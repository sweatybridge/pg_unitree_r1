#include "pg_unitree_r1/control_core.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace pg_unitree_r1;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

LowState state_at(std::uint64_t now_ns) {
  LowState state;
  state.crc_valid = true;
  state.received_at_ns = now_ns;
  state.mode_machine = 7;
  for (std::size_t i = 0; i < kMotorCount; ++i) {
    state.q[i] = static_cast<float>(i) / 100.0F;
  }
  return state;
}

LowTarget target_at(std::uint64_t generation, std::uint64_t revision,
                    std::uint64_t deadline_ns) {
  LowTarget target;
  target.generation = generation;
  target.revision = revision;
  target.valid_until_ns = deadline_ns;
  target.kp.fill(100.0F);
  target.kd.fill(3.0F);
  return target;
}

void mode_exclusivity() {
  ControlCore core;
  core.set_ready();
  check(core.begin_high_level(), "high-level command starts while ready");
  core.finish_high_level();
  check(core.arm_low_level(42, state_at(1'000), 1'000),
        "fresh state arms low-level control");
  check(!core.begin_high_level(),
        "high-level command is rejected while low-level is armed");
  check(core.activate_low_level(), "armed low-level session activates");
  check(!core.begin_high_level(),
        "high-level command is rejected while low-level is active");
}

void target_fencing_and_validation() {
  ControlCore core;
  core.set_ready();
  check(core.arm_low_level(9, state_at(10), 10), "session arms");
  check(core.activate_low_level(), "session activates");

  auto wrong_generation = target_at(8, 1, 1'000);
  check(core.accept_target(wrong_generation, 20) ==
            TargetError::kWrongGeneration,
        "old session generation is fenced out");

  auto first = target_at(9, 1, 1'000);
  check(core.accept_target(first, 20) == TargetError::kNone,
        "first target is accepted");
  check(core.accept_target(first, 21) == TargetError::kStaleRevision,
        "duplicate target revision is rejected");

  auto invalid = target_at(9, 2, 1'000);
  invalid.tau[4] = std::numeric_limits<float>::quiet_NaN();
  check(core.accept_target(invalid, 22) == TargetError::kNonFinite,
        "non-finite actuator values are rejected");
}

void watchdog_faults_to_damping() {
  SafetyLimits limits;
  limits.state_timeout_ns = 100;
  ControlCore core(limits);
  core.set_ready();
  check(core.arm_low_level(3, state_at(100), 100), "session arms");
  check(core.activate_low_level(), "session activates");
  check(core.accept_target(target_at(3, 1, 1'000), 110) ==
            TargetError::kNone,
        "target is accepted");

  const auto healthy = core.tick(state_at(150), 150);
  check(!healthy.faulted && healthy.frame.enabled,
        "fresh state produces an enabled control frame");
  check(!healthy.frame.damping, "healthy target is not damping");

  const auto stale = core.tick(state_at(150), 251);
  check(stale.faulted, "stale robot state trips the watchdog");
  check(stale.fault_code == "state_timeout", "fault is observable by code");
  check(stale.frame.enabled && stale.frame.damping,
        "watchdog fault produces a bounded damping frame");
  check(core.mode() == GatewayMode::kFaulted,
        "watchdog fault is latched in the core state");
}

void expired_target_faults() {
  ControlCore core;
  core.set_ready();
  check(core.arm_low_level(4, state_at(500), 500), "session arms");
  check(core.activate_low_level(), "session activates");
  check(core.accept_target(target_at(4, 1, 550), 510) ==
            TargetError::kNone,
        "unexpired target is accepted");
  const auto expired = core.tick(state_at(600), 600);
  check(expired.faulted && expired.fault_code == "target_timeout",
        "expired target causes an explicit target-timeout fault");
  check(expired.frame.damping, "expired target falls back to damping");
}

}  // namespace

int main() {
  mode_exclusivity();
  target_fencing_and_validation();
  watchdog_faults_to_damping();
  expired_target_faults();
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "control_core_test: all checks passed\n";
  return EXIT_SUCCESS;
}
