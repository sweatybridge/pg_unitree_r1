#pragma once

#include "pg_unitree_r1/control_core.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace pg_unitree_r1 {

enum class HighCommand {
  kStart,
  kStandUp,
  kDamp,
  kZeroTorque,
  kStopMove,
  kMove,
  kSetVelocity,
  kSetSpeedMode,
};

struct HighCommandArgs {
  float vx = 0.0F;
  float vy = 0.0F;
  float yaw = 0.0F;
  float duration_s = 1.0F;
  int integer_arg = 0;
};

struct GatewayResult {
  bool ok = false;
  int sdk_code = 0;
  std::string error_code;
  std::string detail;
};

struct GatewaySnapshot {
  GatewayMode mode = GatewayMode::kOffline;
  std::uint64_t active_generation = 0;
  std::uint64_t state_received_at_ns = 0;
  std::uint64_t robot_tick = 0;
  std::uint64_t control_ticks = 0;
  std::uint64_t deadline_misses = 0;
  std::uint64_t crc_errors = 0;
  std::uint64_t publish_failures = 0;
  std::string fault_code;
  LowState latest_state{};
  bool has_state = false;
};

class UnitreeGateway {
 public:
  UnitreeGateway();
  ~UnitreeGateway();
  UnitreeGateway(const UnitreeGateway&) = delete;
  UnitreeGateway& operator=(const UnitreeGateway&) = delete;

  GatewayResult initialize(const std::string& network_interface,
                           float sdk_timeout_s);
  GatewayResult execute(HighCommand command, const HighCommandArgs& args);
  GatewayResult prepare_low_level(std::uint64_t generation,
                                  std::uint32_t state_timeout_ms,
                                  std::uint32_t damping_ms);
  GatewayResult update_target(const LowTarget& target);
  GatewayResult start_low_level();
  GatewayResult stop_low_level();
  GatewaySnapshot snapshot() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

std::uint64_t monotonic_now_ns() noexcept;

}  // namespace pg_unitree_r1
