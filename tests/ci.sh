#!/usr/bin/env bash
set -Eeuo pipefail

image="${1:?usage: ci.sh IMAGE PG_MAJOR}"
pg_major="${2:?usage: ci.sh IMAGE PG_MAJOR}"
extension_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sql_contract="${extension_dir}/tests/sql_contract.sql"
container="pg-unitree-r1-ci-${pg_major}-${RANDOM}"

case "$(uname -s)" in
  MINGW* | MSYS*)
    export MSYS_NO_PATHCONV=1
    sql_contract="$(cygpath -w "${sql_contract}")"
    ;;
esac

cleanup() {
  docker rm --force "${container}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

wait_for_postgres() {
  for _ in $(seq 1 60); do
    if docker exec "${container}" pg_isready -U postgres -d postgres \
        >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  docker logs "${container}"
  return 1
}

start_postgres() {
  docker run --detach --name "${container}" \
    --env POSTGRES_PASSWORD=test "${image}" "$@" >/dev/null
  wait_for_postgres
}

stop_postgres() {
  docker stop "${container}" >/dev/null
  docker rm "${container}" >/dev/null
}

wait_for_query() {
  local expected="$1"
  local query="$2"
  local actual=""
  for _ in $(seq 1 40); do
    actual="$(docker exec "${container}" psql -X -U postgres -d postgres \
      -Atqc "${query}" 2>/dev/null || true)"
    if [[ "${actual}" == "${expected}" ]]; then
      return 0
    fi
    sleep 0.5
  done
  echo "expected query result: ${expected}" >&2
  echo "actual query result:   ${actual}" >&2
  docker logs "${container}" >&2
  return 1
}

docker run --rm "${image}" /src/build/control_core_test

start_postgres
docker cp "${sql_contract}" \
  "${container}:/tmp/sql_contract.sql"
docker exec "${container}" psql -X -U postgres -d postgres \
  -v ON_ERROR_STOP=1 -f /tmp/sql_contract.sql
stop_postgres

start_postgres -c shared_preload_libraries=pg_unitree_r1
docker exec "${container}" psql -X -U postgres -d postgres \
  -v ON_ERROR_STOP=1 -c "CREATE EXTENSION pg_unitree_r1"
wait_for_query \
  "offline|network_interface_required|true|1" \
  "SELECT state || '|' || COALESCE(last_error_code, '') || '|' ||
          (heartbeat_age < interval '2500 milliseconds')::text || '|' ||
          (SELECT count(*) FROM pg_stat_activity
            WHERE backend_type = 'pg_unitree_r1 gateway')::text
     FROM unitree_r1.health()"
stop_postgres

start_postgres \
  -c shared_preload_libraries=pg_unitree_r1 \
  -c pg_unitree_r1.network_interface=lo \
  -c pg_unitree_r1.sdk_timeout_s=2
docker exec "${container}" psql -X -U postgres -d postgres \
  -v ON_ERROR_STOP=1 -c "CREATE EXTENSION pg_unitree_r1"
wait_for_query \
  "ready|true|1" \
  "SELECT state || '|' || healthy::text || '|' ||
          (SELECT count(*) FROM pg_stat_activity
            WHERE backend_type = 'pg_unitree_r1 gateway')::text
     FROM unitree_r1.health()"
command_id="$(docker exec "${container}" psql -X -U postgres -d postgres \
  -Atqc "SELECT unitree_r1.stop_move('ci/loopback-stop')")"
wait_for_query \
  "failed|sdk_error|ready|true|1" \
  "SELECT c.status || '|' || COALESCE(c.error_code, '') || '|' ||
          h.state || '|' || h.healthy::text || '|' ||
          (SELECT count(*) FROM pg_stat_activity
            WHERE backend_type = 'pg_unitree_r1 gateway')::text
     FROM unitree_r1.command_status(${command_id}) c
     CROSS JOIN unitree_r1.health() h"
stop_postgres

echo "ci smoke tests passed for PostgreSQL ${pg_major}"
