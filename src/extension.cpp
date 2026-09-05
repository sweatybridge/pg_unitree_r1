extern "C" {
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/procsignal.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;
}

#include "pg_unitree_r1/unitree_gateway.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

volatile sig_atomic_t got_sigterm = false;
volatile sig_atomic_t got_sighup = false;
char* configured_database = nullptr;
char* configured_network_interface = nullptr;
bool configured_enabled = true;
int configured_poll_ms = 10;
double configured_sdk_timeout_s = 5.0;

struct ClaimedCommand {
  std::int64_t id = 0;
  std::string kind;
  std::int64_t session_id = 0;
  pg_unitree_r1::HighCommandArgs args{};
};

struct SessionConfig {
  std::int64_t id = 0;
  std::uint64_t generation = 0;
  std::uint32_t state_timeout_ms = 100;
  std::uint32_t damping_ms = 250;
  std::string status;
  std::int64_t last_target_id = 0;
};

struct PendingTarget {
  std::int64_t id = 0;
  pg_unitree_r1::LowTarget target{};
  bool expired = false;
};

void signal_term(SIGNAL_ARGS) {
  const int saved_errno = errno;
  got_sigterm = true;
  SetLatch(MyLatch);
  errno = saved_errno;
}

void signal_hup(SIGNAL_ARGS) {
  const int saved_errno = errno;
  got_sighup = true;
  SetLatch(MyLatch);
  errno = saved_errno;
}

void ensure_spi(int code, int expected, const char* operation) {
  if (code != expected) {
    throw std::runtime_error(std::string(operation) + " failed with SPI code " +
                             std::to_string(code));
  }
}

int execute_with_args(const char* query, int count, const Oid* types,
                      const Datum* values, const char* nulls, bool read_only,
                      long row_count) {
  return SPI_execute_with_args(query, count, const_cast<Oid*>(types),
                               const_cast<Datum*>(values), nulls, read_only,
                               row_count);
}

template <typename Callback>
void with_spi_transaction(Callback&& callback) {
  StartTransactionCommand();
  PushActiveSnapshot(GetTransactionSnapshot());
  bool connected = false;
  bool snapshot_pushed = true;
  try {
    ensure_spi(SPI_connect(), SPI_OK_CONNECT, "SPI_connect");
    connected = true;
    callback();
    ensure_spi(SPI_finish(), SPI_OK_FINISH, "SPI_finish");
    connected = false;
    PopActiveSnapshot();
    snapshot_pushed = false;
    CommitTransactionCommand();
  } catch (...) {
    if (connected) {
      SPI_finish();
    }
    if (snapshot_pushed) {
      PopActiveSnapshot();
    }
    AbortCurrentTransaction();
    throw;
  }
}

Datum value_at(HeapTuple tuple, TupleDesc descriptor, int column,
               bool& is_null) {
  return SPI_getbinval(tuple, descriptor, column, &is_null);
}

std::string text_at(HeapTuple tuple, TupleDesc descriptor, int column) {
  bool is_null = false;
  const Datum value = value_at(tuple, descriptor, column, is_null);
  if (is_null) {
    return {};
  }
  char* buffer = TextDatumGetCString(value);
  std::string result(buffer);
  pfree(buffer);
  return result;
}

std::int64_t int64_at(HeapTuple tuple, TupleDesc descriptor, int column,
                      std::int64_t fallback = 0) {
  bool is_null = false;
  const Datum value = value_at(tuple, descriptor, column, is_null);
  return is_null ? fallback : DatumGetInt64(value);
}

std::int32_t int32_at(HeapTuple tuple, TupleDesc descriptor, int column,
                      std::int32_t fallback = 0) {
  bool is_null = false;
  const Datum value = value_at(tuple, descriptor, column, is_null);
  return is_null ? fallback : DatumGetInt32(value);
}

float float4_at(HeapTuple tuple, TupleDesc descriptor, int column,
                float fallback = 0.0F) {
  bool is_null = false;
  const Datum value = value_at(tuple, descriptor, column, is_null);
  return is_null ? fallback : DatumGetFloat4(value);
}

double float8_at(HeapTuple tuple, TupleDesc descriptor, int column,
                 double fallback = 0.0) {
  bool is_null = false;
  const Datum value = value_at(tuple, descriptor, column, is_null);
  return is_null ? fallback : DatumGetFloat8(value);
}

pg_unitree_r1::JointVector float_array_at(HeapTuple tuple,
                                          TupleDesc descriptor, int column) {
  bool is_null = false;
  const Datum value = value_at(tuple, descriptor, column, is_null);
  if (is_null) {
    throw std::runtime_error("low-level target array is null");
  }
  ArrayType* array = DatumGetArrayTypeP(value);
  Datum* values = nullptr;
  bool* nulls = nullptr;
  int count = 0;
  deconstruct_array(array, FLOAT4OID, sizeof(float), true, TYPALIGN_INT,
                    &values, &nulls, &count);
  if (count != static_cast<int>(pg_unitree_r1::kMotorCount)) {
    throw std::runtime_error("low-level target array does not have 26 values");
  }
  pg_unitree_r1::JointVector result{};
  for (int i = 0; i < count; ++i) {
    if (nulls[i]) {
      throw std::runtime_error("low-level target array contains null");
    }
    result[static_cast<std::size_t>(i)] = DatumGetFloat4(values[i]);
  }
  pfree(values);
  pfree(nulls);
  return result;
}

std::optional<ClaimedCommand> claim_command() {
  std::optional<ClaimedCommand> command;
  with_spi_transaction([&] {
    const char* query =
        "WITH next AS ("
        "  SELECT id FROM unitree_r1.command"
        "   WHERE status = 'queued' ORDER BY id"
        "   FOR UPDATE SKIP LOCKED LIMIT 1"
        ") "
        "UPDATE unitree_r1.command c"
        "   SET status = 'running', started_at = clock_timestamp(),"
        "       attempt = attempt + 1"
        "  FROM next WHERE c.id = next.id"
        " RETURNING c.id, c.kind, c.session_id, c.vx, c.vy, c.yaw,"
        "           c.duration_s, c.integer_arg";
    ensure_spi(SPI_execute(query, false, 1), SPI_OK_UPDATE_RETURNING,
               "claim command");
    if (SPI_processed == 0) {
      return;
    }
    const auto tuple = SPI_tuptable->vals[0];
    const auto descriptor = SPI_tuptable->tupdesc;
    ClaimedCommand claimed;
    claimed.id = int64_at(tuple, descriptor, 1);
    claimed.kind = text_at(tuple, descriptor, 2);
    claimed.session_id = int64_at(tuple, descriptor, 3);
    claimed.args.vx = float4_at(tuple, descriptor, 4);
    claimed.args.vy = float4_at(tuple, descriptor, 5);
    claimed.args.yaw = float4_at(tuple, descriptor, 6);
    claimed.args.duration_s = float4_at(tuple, descriptor, 7, 1.0F);
    claimed.args.integer_arg = int32_at(tuple, descriptor, 8);
    command = std::move(claimed);
  });
  return command;
}

bool extension_installed() {
  bool installed = false;
  with_spi_transaction([&] {
    ensure_spi(
        SPI_execute(
            "SELECT to_regclass('unitree_r1.gateway_status') IS NOT NULL",
            true, 1),
        SPI_OK_SELECT, "check extension installation");
    if (SPI_processed == 1) {
      bool is_null = false;
      installed = DatumGetBool(value_at(SPI_tuptable->vals[0],
                                         SPI_tuptable->tupdesc, 1, is_null));
      installed = installed && !is_null;
    }
  });
  return installed;
}

std::optional<SessionConfig> load_session(std::int64_t session_id) {
  std::optional<SessionConfig> session;
  with_spi_transaction([&] {
    const Oid types[] = {INT8OID};
    const Datum values[] = {Int64GetDatum(session_id)};
    const char nulls[] = {' '};
    const char* query =
        "SELECT id, generation, state_timeout_ms, damping_ms, status,"
        "       COALESCE(last_target_id, 0)"
        "  FROM unitree_r1.low_level_session WHERE id = $1";
    ensure_spi(execute_with_args(query, 1, types, values, nulls, true, 1),
               SPI_OK_SELECT, "load low-level session");
    if (SPI_processed == 0) {
      return;
    }
    const auto tuple = SPI_tuptable->vals[0];
    const auto descriptor = SPI_tuptable->tupdesc;
    SessionConfig loaded;
    loaded.id = int64_at(tuple, descriptor, 1);
    loaded.generation =
        static_cast<std::uint64_t>(int64_at(tuple, descriptor, 2));
    loaded.state_timeout_ms =
        static_cast<std::uint32_t>(int32_at(tuple, descriptor, 3));
    loaded.damping_ms =
        static_cast<std::uint32_t>(int32_at(tuple, descriptor, 4));
    loaded.status = text_at(tuple, descriptor, 5);
    loaded.last_target_id = int64_at(tuple, descriptor, 6);
    session = std::move(loaded);
  });
  return session;
}

std::optional<PendingTarget> load_pending_target(std::int64_t session_id,
                                                 std::int64_t after_id) {
  std::optional<PendingTarget> target;
  with_spi_transaction([&] {
    const Oid types[] = {INT8OID, INT8OID};
    const Datum values[] = {Int64GetDatum(session_id), Int64GetDatum(after_id)};
    const char nulls[] = {' ', ' '};
    const char* query =
        "SELECT id, generation, revision, q, dq, kp, kd, tau,"
        "       EXTRACT(epoch FROM (valid_until - clock_timestamp()))::float8"
        "  FROM unitree_r1.low_level_target"
        " WHERE session_id = $1 AND id > $2 AND status = 'queued'"
        " ORDER BY id DESC LIMIT 1";
    ensure_spi(execute_with_args(query, 2, types, values, nulls, true, 1),
               SPI_OK_SELECT, "load pending low-level target");
    if (SPI_processed == 0) {
      return;
    }
    const auto tuple = SPI_tuptable->vals[0];
    const auto descriptor = SPI_tuptable->tupdesc;
    PendingTarget loaded;
    loaded.id = int64_at(tuple, descriptor, 1);
    loaded.target.generation =
        static_cast<std::uint64_t>(int64_at(tuple, descriptor, 2));
    loaded.target.revision =
        static_cast<std::uint64_t>(int64_at(tuple, descriptor, 3));
    loaded.target.q = float_array_at(tuple, descriptor, 4);
    loaded.target.dq = float_array_at(tuple, descriptor, 5);
    loaded.target.kp = float_array_at(tuple, descriptor, 6);
    loaded.target.kd = float_array_at(tuple, descriptor, 7);
    loaded.target.tau = float_array_at(tuple, descriptor, 8);
    const double ttl_s = float8_at(tuple, descriptor, 9);
    loaded.expired = ttl_s <= 0.0;
    const double bounded_ttl_s = std::clamp(ttl_s, 0.0, 1.0);
    loaded.target.valid_until_ns = pg_unitree_r1::monotonic_now_ns() +
        static_cast<std::uint64_t>(bounded_ttl_s * 1'000'000'000.0);
    target = std::move(loaded);
  });
  return target;
}

void complete_command(std::int64_t id, const std::string& status,
                      const pg_unitree_r1::GatewayResult& result) {
  with_spi_transaction([&] {
    const Oid types[] = {INT8OID, TEXTOID, INT4OID, TEXTOID, TEXTOID, TEXTOID};
    const Datum values[] = {
        Int64GetDatum(id),
        CStringGetTextDatum(status.c_str()),
        Int32GetDatum(result.sdk_code),
        CStringGetTextDatum(result.ok ? result.detail.c_str() : ""),
        CStringGetTextDatum(result.error_code.c_str()),
        CStringGetTextDatum(result.ok ? "" : result.detail.c_str())};
    const char nulls[] = {' ', ' ', ' ', ' ', ' ', ' '};
    const char* query =
        "UPDATE unitree_r1.command"
        "   SET status = $2, sdk_code = $3, result = NULLIF($4, ''),"
        "       error_code = NULLIF($5, ''), error_detail = NULLIF($6, ''),"
        "       completed_at = clock_timestamp()"
        " WHERE id = $1";
    ensure_spi(execute_with_args(query, 6, types, values, nulls, false, 0),
               SPI_OK_UPDATE, "complete command");
  });
}

void set_session_status(std::int64_t session_id, const std::string& status,
                        const std::string& error_code = {},
                        const std::string& error_detail = {}) {
  with_spi_transaction([&] {
    const Oid types[] = {INT8OID, TEXTOID, TEXTOID, TEXTOID};
    const Datum values[] = {
        Int64GetDatum(session_id), CStringGetTextDatum(status.c_str()),
        CStringGetTextDatum(error_code.c_str()),
        CStringGetTextDatum(error_detail.c_str())};
    const char nulls[] = {' ', ' ', ' ', ' '};
    const char* query =
        "UPDATE unitree_r1.low_level_session"
        "   SET status = $2,"
        "       armed_at = CASE WHEN $2 = 'armed' THEN clock_timestamp() ELSE armed_at END,"
        "       started_at = CASE WHEN $2 = 'active' THEN clock_timestamp() ELSE started_at END,"
        "       stopped_at = CASE WHEN $2 IN ('stopped','faulted')"
        "                         THEN clock_timestamp() ELSE stopped_at END,"
        "       fault_code = NULLIF($3, ''), fault_detail = NULLIF($4, '')"
        " WHERE id = $1";
    ensure_spi(execute_with_args(query, 4, types, values, nulls, false, 0),
               SPI_OK_UPDATE, "update low-level session");
  });
}

void set_target_status(const PendingTarget& target, const std::string& status,
                       const std::string& error_code) {
  with_spi_transaction([&] {
    const Oid types[] = {INT8OID, INT8OID, TEXTOID, TEXTOID};
    const Datum values[] = {
        Int64GetDatum(target.id), Int64GetDatum(target.id),
        CStringGetTextDatum(status.c_str()),
        CStringGetTextDatum(error_code.c_str())};
    const char nulls[] = {' ', ' ', ' ', ' '};
    const char* target_query =
        "UPDATE unitree_r1.low_level_target"
        "   SET status = CASE WHEN id = $1 THEN $3 ELSE 'rejected' END,"
        "       error_code = CASE WHEN id = $1 THEN NULLIF($4, '')"
        "                         ELSE 'superseded' END"
        " WHERE session_id = (SELECT session_id FROM unitree_r1.low_level_target"
        "                      WHERE id = $1)"
        "   AND status = 'queued' AND id <= $2";
    ensure_spi(execute_with_args(target_query, 4, types, values, nulls,
                                     false, 0),
               SPI_OK_UPDATE, "update low-level target");

    const Oid session_types[] = {INT8OID, INT8OID, TEXTOID};
    const Datum session_values[] = {
        Int64GetDatum(target.id),
        Int64GetDatum(static_cast<std::int64_t>(target.target.revision)),
        CStringGetTextDatum(status.c_str())};
    const char session_nulls[] = {' ', ' ', ' '};
    const char* session_query =
        "UPDATE unitree_r1.low_level_session"
        "   SET last_target_id = CASE WHEN $3 = 'accepted' THEN $1 ELSE last_target_id END,"
        "       last_target_revision = CASE WHEN $3 = 'accepted' THEN $2"
        "                                   ELSE last_target_revision END"
        " WHERE id = (SELECT session_id FROM unitree_r1.low_level_target"
        "             WHERE id = $1)";
    ensure_spi(execute_with_args(session_query, 3, session_types,
                                     session_values, session_nulls, false, 0),
               SPI_OK_UPDATE, "update low-level target");
  });
}

Datum make_float_array(const pg_unitree_r1::JointVector& vector) {
  Datum values[pg_unitree_r1::kMotorCount];
  for (std::size_t i = 0; i < pg_unitree_r1::kMotorCount; ++i) {
    values[i] = Float4GetDatum(vector[i]);
  }
  return PointerGetDatum(construct_array(
      values, static_cast<int>(pg_unitree_r1::kMotorCount), FLOAT4OID,
      sizeof(float), true, TYPALIGN_INT));
}

void update_observability(const pg_unitree_r1::GatewaySnapshot& snapshot,
                          const std::string& display_state,
                          const std::string& network_interface,
                          std::int64_t active_session_id,
                          std::uint64_t active_generation,
                          const std::string& last_error_code,
                          const std::string& last_error_detail,
                          bool state_changed,
                          bool write_telemetry) {
  with_spi_transaction([&] {
    const Oid status_types[] = {TEXTOID, TEXTOID, INT8OID, INT8OID, INT8OID,
                                INT8OID, INT8OID, INT8OID, TEXTOID, TEXTOID,
                                BOOLOID};
    const Datum status_values[] = {
        CStringGetTextDatum(display_state.c_str()),
        CStringGetTextDatum(network_interface.c_str()),
        Int64GetDatum(active_session_id),
        Int64GetDatum(static_cast<std::int64_t>(active_generation)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.control_ticks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.deadline_misses)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.crc_errors)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.publish_failures)),
        CStringGetTextDatum(last_error_code.c_str()),
        CStringGetTextDatum(last_error_detail.c_str()),
        BoolGetDatum(state_changed)};
    const char status_nulls[] = {' ', ' ', ' ', ' ', ' ',
                                 ' ', ' ', ' ', ' ', ' ', ' '};
    const char* status_query =
        "UPDATE unitree_r1.gateway_status"
        "   SET worker_pid = pg_backend_pid(), state = $1, network_interface = $2,"
        "       heartbeat_at = clock_timestamp(),"
        "       last_state_at = CASE WHEN $11 THEN clock_timestamp()"
        "                            ELSE last_state_at END,"
        "       active_session_id = NULLIF($3, 0),"
        "       active_generation = NULLIF($4, 0),"
        "       control_ticks = $5, deadline_misses = $6, crc_errors = $7,"
        "       publish_failures = $8,"
        "       last_error_code = CASE WHEN $9 = '' THEN last_error_code ELSE $9 END,"
        "       last_error_detail = CASE WHEN $10 = '' THEN last_error_detail ELSE $10 END,"
        "       last_error_at = CASE WHEN $9 = '' THEN last_error_at"
        "                            ELSE clock_timestamp() END"
        " WHERE singleton";
    ensure_spi(execute_with_args(status_query, 11, status_types,
                                     status_values, status_nulls, false, 0),
               SPI_OK_UPDATE, "update gateway status");

    if (!write_telemetry || !snapshot.has_state) {
      return;
    }
    const Oid telemetry_types[] = {INT8OID, INT8OID, INT8OID, INT4OID,
                                   FLOAT4ARRAYOID, FLOAT4ARRAYOID};
    const Datum telemetry_values[] = {
        Int64GetDatum(active_session_id),
        Int64GetDatum(static_cast<std::int64_t>(active_generation)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.robot_tick)),
        Int32GetDatum(snapshot.latest_state.mode_machine),
        make_float_array(snapshot.latest_state.q),
        make_float_array(snapshot.latest_state.dq)};
    const char telemetry_nulls[] = {' ', ' ', ' ', ' ', ' ', ' '};
    const char* telemetry_query =
        "INSERT INTO unitree_r1.telemetry_latest("
        " singleton, observed_at, session_id, generation, robot_tick,"
        " mode_machine, q, dq)"
        " VALUES (true, clock_timestamp(), NULLIF($1, 0), NULLIF($2, 0),"
        "         $3, $4, $5, $6)"
        " ON CONFLICT (singleton) DO UPDATE SET"
        " observed_at = EXCLUDED.observed_at, session_id = EXCLUDED.session_id,"
        " generation = EXCLUDED.generation, robot_tick = EXCLUDED.robot_tick,"
        " mode_machine = EXCLUDED.mode_machine, q = EXCLUDED.q, dq = EXCLUDED.dq";
    ensure_spi(execute_with_args(telemetry_query, 6, telemetry_types,
                                     telemetry_values, telemetry_nulls, false, 0),
               SPI_OK_INSERT, "update telemetry");
  });
}

void mark_worker_starting(const std::string& network_interface) {
  with_spi_transaction([&] {
    ensure_spi(
        SPI_execute(
            "UPDATE unitree_r1.command SET status = 'ambiguous',"
            " error_code = 'gateway_restarted',"
            " error_detail = 'gateway restarted while command was running',"
            " completed_at = clock_timestamp() WHERE status = 'running'",
            false, 0),
        SPI_OK_UPDATE, "fence interrupted commands");
    ensure_spi(
        SPI_execute(
            "UPDATE unitree_r1.low_level_session SET status = 'faulted',"
            " fault_code = 'gateway_restarted',"
            " fault_detail = 'low-level control never resumes automatically',"
            " stopped_at = clock_timestamp()"
            " WHERE status IN ('preparing','armed','active','stopping')",
            false, 0),
        SPI_OK_UPDATE, "fence interrupted low-level sessions");
    const Oid types[] = {TEXTOID};
    const Datum values[] = {CStringGetTextDatum(network_interface.c_str())};
    const char nulls[] = {' '};
    const char* query =
        "UPDATE unitree_r1.gateway_status SET worker_pid = pg_backend_pid(),"
        " state = 'starting', network_interface = $1,"
        " started_at = clock_timestamp(), heartbeat_at = clock_timestamp(),"
        " active_session_id = NULL, active_generation = NULL WHERE singleton";
    ensure_spi(execute_with_args(query, 1, types, values, nulls, false, 0),
               SPI_OK_UPDATE, "initialize gateway status");
  });
}

void mark_worker_offline(const std::string& error_code,
                         const std::string& error_detail) {
  with_spi_transaction([&] {
    const Oid types[] = {TEXTOID, TEXTOID};
    const Datum values[] = {CStringGetTextDatum(error_code.c_str()),
                            CStringGetTextDatum(error_detail.c_str())};
    const char nulls[] = {' ', ' '};
    const char* query =
        "UPDATE unitree_r1.gateway_status SET state = 'offline',"
        " heartbeat_at = clock_timestamp(), active_session_id = NULL,"
        " active_generation = NULL, last_error_code = NULLIF($1, ''),"
        " last_error_detail = NULLIF($2, ''),"
        " last_error_at = CASE WHEN $1 = '' THEN last_error_at"
        "                      ELSE clock_timestamp() END WHERE singleton";
    ensure_spi(execute_with_args(query, 2, types, values, nulls, false, 0),
               SPI_OK_UPDATE, "mark gateway offline");
  });
}

std::optional<pg_unitree_r1::HighCommand> parse_high_command(
    const std::string& kind) {
  using pg_unitree_r1::HighCommand;
  if (kind == "start") return HighCommand::kStart;
  if (kind == "stand_up") return HighCommand::kStandUp;
  if (kind == "damp") return HighCommand::kDamp;
  if (kind == "zero_torque") return HighCommand::kZeroTorque;
  if (kind == "stop_move") return HighCommand::kStopMove;
  if (kind == "move") return HighCommand::kMove;
  if (kind == "set_velocity") return HighCommand::kSetVelocity;
  if (kind == "set_speed_mode") return HighCommand::kSetSpeedMode;
  return std::nullopt;
}

std::string command_failure_status(const pg_unitree_r1::GatewayResult& result) {
  if (result.error_code == "mode_conflict" ||
      result.error_code == "wrong_mode" ||
      result.error_code == "wrong_generation" ||
      result.error_code == "stale_revision" ||
      result.error_code == "expired" ||
      result.error_code == "non_finite" ||
      result.error_code == "out_of_range") {
    return "rejected";
  }
  if (result.error_code == "sdk_exception") {
    return "ambiguous";
  }
  return "failed";
}

}  // namespace

extern "C" PGDLLEXPORT void pg_unitree_r1_worker_main(Datum);
extern "C" PGDLLEXPORT void _PG_init(void);

extern "C" PGDLLEXPORT void _PG_init(void) {
  DefineCustomBoolVariable(
      "pg_unitree_r1.enabled",
      "Start the embedded Unitree R1 gateway background worker.", nullptr,
      &configured_enabled, true, PGC_POSTMASTER, 0, nullptr, nullptr, nullptr);
  DefineCustomStringVariable(
      "pg_unitree_r1.database",
      "Database containing the pg_unitree_r1 extension.", nullptr,
      &configured_database, "postgres", PGC_POSTMASTER, 0, nullptr, nullptr,
      nullptr);
  DefineCustomStringVariable(
      "pg_unitree_r1.network_interface",
      "Network interface used for Unitree DDS traffic.", nullptr,
      &configured_network_interface, "", PGC_POSTMASTER, 0, nullptr, nullptr,
      nullptr);
  DefineCustomIntVariable(
      "pg_unitree_r1.poll_ms", "Command and target queue polling interval.",
      nullptr, &configured_poll_ms, 10, 2, 1000, PGC_SIGHUP, GUC_UNIT_MS,
      nullptr, nullptr, nullptr);
  DefineCustomRealVariable(
      "pg_unitree_r1.sdk_timeout_s", "Timeout for synchronous Unitree RPCs.",
      nullptr, &configured_sdk_timeout_s, 5.0, 0.1, 60.0, PGC_SIGHUP, 0,
      nullptr, nullptr, nullptr);

  if (!process_shared_preload_libraries_in_progress || !configured_enabled) {
    return;
  }
  BackgroundWorker worker{};
  worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
  worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
  worker.bgw_restart_time = 5;
  std::snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_unitree_r1");
  std::snprintf(worker.bgw_function_name, BGW_MAXLEN,
                "pg_unitree_r1_worker_main");
  std::snprintf(worker.bgw_name, BGW_MAXLEN, "pg_unitree_r1 gateway");
  std::snprintf(worker.bgw_type, BGW_MAXLEN, "pg_unitree_r1 gateway");
  worker.bgw_main_arg = static_cast<Datum>(0);
  worker.bgw_notify_pid = 0;
  RegisterBackgroundWorker(&worker);
}

extern "C" PGDLLEXPORT void pg_unitree_r1_worker_main(Datum) {
  pqsignal(SIGTERM, signal_term);
  pqsignal(SIGHUP, signal_hup);
  BackgroundWorkerUnblockSignals();
  ereport(LOG,
          (errmsg("pg_unitree_r1 event=worker_boot phase=connecting database=%s",
                  configured_database)));
  BackgroundWorkerInitializeConnection(configured_database, nullptr, 0);
  ereport(LOG, (errmsg("pg_unitree_r1 event=worker_boot phase=connected")));

  const std::string network_interface =
      configured_network_interface == nullptr ? "" : configured_network_interface;
  std::unique_ptr<pg_unitree_r1::UnitreeGateway> gateway;
  std::int64_t active_session_id = 0;
  std::uint64_t active_generation = 0;
  std::int64_t last_target_id = 0;
  std::string display_state = "starting";
  std::string last_error_code;
  std::string last_error_detail;
  std::uint64_t last_observability_ns = 0;
  std::uint64_t last_telemetry_ns = 0;
  std::uint64_t last_persisted_state_ns = 0;
  bool postmaster_died = false;

  try {
    while (!got_sigterm && !extension_installed()) {
      const int latch_events = WaitLatch(
          MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 1000,
          PG_WAIT_EXTENSION);
      ResetLatch(MyLatch);
      if ((latch_events & WL_POSTMASTER_DEATH) != 0) {
        return;
      }
      CHECK_FOR_INTERRUPTS();
    }
    if (got_sigterm) {
      return;
    }
    mark_worker_starting(network_interface);
    if (network_interface.empty()) {
      constexpr const char* error_code = "network_interface_required";
      constexpr const char* error_detail =
          "set pg_unitree_r1.network_interface at PostgreSQL startup";
      ereport(WARNING,
              (errmsg("pg_unitree_r1 event=configuration_required code=%s detail=%s",
                      error_code, error_detail)));
      while (!got_sigterm) {
        if (got_sighup) {
          got_sighup = false;
          ProcessConfigFile(PGC_SIGHUP);
        }
        mark_worker_offline(error_code, error_detail);
        const int latch_events = WaitLatch(
            MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 1000,
            PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
        if ((latch_events & WL_POSTMASTER_DEATH) != 0) {
          return;
        }
        CHECK_FOR_INTERRUPTS();
      }
      return;
    }

    // Unitree client construction may touch SDK process state. Defer it until
    // the extension schema exists and configuration has passed validation.
    gateway = std::make_unique<pg_unitree_r1::UnitreeGateway>();
    ereport(LOG,
            (errmsg("pg_unitree_r1 event=worker_start database=%s interface=%s",
                    configured_database, network_interface.c_str())));
    const auto initialized = gateway->initialize(
        network_interface, static_cast<float>(configured_sdk_timeout_s));
    if (!initialized.ok) {
      mark_worker_offline(initialized.error_code, initialized.detail);
      ereport(ERROR,
              (errmsg("pg_unitree_r1 event=initialization_failed code=%s detail=%s",
                      initialized.error_code.c_str(), initialized.detail.c_str())));
    }
    display_state = "ready";

    while (!got_sigterm) {
      if (got_sighup) {
        got_sighup = false;
        ProcessConfigFile(PGC_SIGHUP);
      }
      const auto snapshot_before = gateway->snapshot();
      if (active_session_id != 0 &&
          snapshot_before.mode == pg_unitree_r1::GatewayMode::kFaulted) {
        last_error_code = snapshot_before.fault_code.empty()
                              ? "low_level_fault"
                              : snapshot_before.fault_code;
        last_error_detail = "low-level control loop entered its damping fallback";
        set_session_status(active_session_id, "faulted", last_error_code,
                           last_error_detail);
        gateway->stop_low_level();
        active_session_id = 0;
        active_generation = 0;
        last_target_id = 0;
        display_state = "faulted";
        ereport(WARNING,
                (errmsg("pg_unitree_r1 event=low_level_fault code=%s",
                        last_error_code.c_str())));
      }

      if (active_session_id != 0) {
        const auto pending =
            load_pending_target(active_session_id, last_target_id);
        if (pending.has_value()) {
          last_target_id = pending->id;
          pg_unitree_r1::GatewayResult target_result;
          if (pending->expired) {
            target_result = {false, 0, "expired",
                             "target expired before the gateway consumed it"};
            set_target_status(*pending, "expired", target_result.error_code);
          } else {
            target_result = gateway->update_target(pending->target);
            set_target_status(*pending,
                              target_result.ok ? "accepted" : "rejected",
                              target_result.error_code);
          }
          if (!target_result.ok) {
            last_error_code = target_result.error_code;
            last_error_detail = target_result.detail;
            ereport(WARNING,
                    (errmsg("pg_unitree_r1 event=target_rejected target_id=%lld code=%s",
                            static_cast<long long>(pending->id),
                            target_result.error_code.c_str())));
          }
        }
      }

      const auto command = claim_command();
      if (command.has_value()) {
        pg_unitree_r1::GatewayResult result;
        if (const auto high = parse_high_command(command->kind); high.has_value()) {
          result = gateway->execute(*high, command->args);
          if (result.ok) {
            display_state = "ready";
          }
        } else if (command->kind == "low_prepare") {
          const auto session = load_session(command->session_id);
          if (!session.has_value()) {
            result = {false, 0, "session_not_found",
                      "low-level session does not exist"};
          } else if (active_session_id != 0) {
            result = {false, 0, "mode_conflict",
                      "another low-level session owns the gateway"};
          } else {
            result = gateway->prepare_low_level(
                session->generation, session->state_timeout_ms,
                session->damping_ms);
            if (result.ok) {
              active_session_id = session->id;
              active_generation = session->generation;
              last_target_id = 0;
              display_state = "low_level_armed";
              set_session_status(session->id, "armed");
            }
          }
          if (!result.ok && session.has_value()) {
            set_session_status(session->id, "faulted", result.error_code,
                               result.detail);
          }
        } else if (command->kind == "low_start") {
          const auto session = load_session(command->session_id);
          if (!session.has_value() || active_session_id != command->session_id ||
              session->generation != active_generation) {
            result = {false, 0, "wrong_generation",
                      "low-level start does not match the armed session"};
          } else if (session->last_target_id == 0) {
            result = {false, 0, "target_missing",
                      "submit an accepted target before starting low-level control"};
          } else {
            result = gateway->start_low_level();
            if (result.ok) {
              display_state = "low_level_active";
              set_session_status(session->id, "active");
            }
          }
        } else if (command->kind == "low_stop") {
          const auto session = load_session(command->session_id);
          if (!session.has_value()) {
            result = {false, 0, "session_not_found",
                      "low-level session does not exist"};
          } else if (active_session_id != command->session_id) {
            result = {false, 0, "wrong_generation",
                      "low-level stop does not match the active session"};
          } else {
            set_session_status(session->id, "stopping");
            result = gateway->stop_low_level();
            if (result.ok) {
              set_session_status(session->id, "stopped");
              active_session_id = 0;
              active_generation = 0;
              last_target_id = 0;
              display_state = "ready";
            }
          }
        } else {
          result = {false, 0, "unknown_command", "unsupported command kind"};
        }

        const std::string final_status =
            result.ok ? "succeeded" : command_failure_status(result);
        complete_command(command->id, final_status, result);
        ereport(result.ok ? LOG : WARNING,
                (errmsg("pg_unitree_r1 event=command_complete command_id=%lld kind=%s status=%s code=%s sdk_code=%d",
                        static_cast<long long>(command->id), command->kind.c_str(),
                        final_status.c_str(), result.error_code.c_str(),
                        result.sdk_code)));
        if (!result.ok) {
          last_error_code = result.error_code;
          last_error_detail = result.detail;
        }
      }

      const auto now_ns = pg_unitree_r1::monotonic_now_ns();
      if (now_ns - last_observability_ns >= 1'000'000'000ULL) {
        const auto snapshot = gateway->snapshot();
        const bool state_changed = snapshot.has_state &&
            snapshot.state_received_at_ns != last_persisted_state_ns;
        const bool write_telemetry = state_changed &&
            now_ns - last_telemetry_ns >= 100'000'000ULL;
        update_observability(snapshot, display_state, network_interface,
                             active_session_id, active_generation,
                             last_error_code, last_error_detail, state_changed,
                             write_telemetry);
        last_observability_ns = now_ns;
        if (state_changed) {
          last_persisted_state_ns = snapshot.state_received_at_ns;
        }
        if (write_telemetry) {
          last_telemetry_ns = now_ns;
        }
        last_error_code.clear();
        last_error_detail.clear();
      }

      const int latch_events = WaitLatch(
          MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
          configured_poll_ms, PG_WAIT_EXTENSION);
      ResetLatch(MyLatch);
      if ((latch_events & WL_POSTMASTER_DEATH) != 0) {
        postmaster_died = true;
        break;
      }
      CHECK_FOR_INTERRUPTS();
    }

    if (active_session_id != 0) {
      gateway->stop_low_level();
      if (!postmaster_died) {
        set_session_status(active_session_id, "faulted", "gateway_stopped",
                           "gateway stopped; low-level control was damped and fenced");
      }
    }
    if (!postmaster_died) {
      mark_worker_offline({}, {});
    }
    ereport(LOG, (errmsg("pg_unitree_r1 event=worker_stop")));
  } catch (const std::exception& error) {
    try {
      if (gateway != nullptr) {
        gateway->stop_low_level();
      }
      mark_worker_offline("worker_exception", error.what());
    } catch (...) {
    }
    ereport(ERROR,
            (errmsg("pg_unitree_r1 event=worker_exception detail=%s",
                    error.what())));
  } catch (...) {
    try {
      if (gateway != nullptr) {
        gateway->stop_low_level();
      }
      mark_worker_offline("worker_exception", "unknown exception");
    } catch (...) {
    }
    ereport(ERROR,
            (errmsg("pg_unitree_r1 event=worker_exception detail=unknown")));
  }
}
