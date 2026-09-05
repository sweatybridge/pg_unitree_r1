#include "pg_unitree_r1/control_core.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pg_unitree_r1 {
namespace {

bool fresh_state(const LowState& state, std::uint64_t now_ns,
                 std::uint64_t timeout_ns) {
  return state.crc_valid && state.received_at_ns <= now_ns &&
         now_ns - state.received_at_ns <= timeout_ns;
}

bool finite_vector(const JointVector& values) {
  return std::all_of(values.begin(), values.end(),
                     [](float value) { return std::isfinite(value); });
}

bool within_symmetric_limit(const JointVector& values, float limit) {
  return std::all_of(values.begin(), values.end(), [limit](float value) {
    return std::abs(value) <= limit;
  });
}

bool within_unsigned_limit(const JointVector& values, float limit) {
  return std::all_of(values.begin(), values.end(), [limit](float value) {
    return value >= 0.0F && value <= limit;
  });
}

ControlFrame damping_frame(const LowState& state) {
  ControlFrame frame;
  frame.q = state.q;
  frame.dq.fill(0.0F);
  frame.kp.fill(0.0F);
  frame.kd.fill(3.0F);
  frame.tau.fill(0.0F);
  frame.mode_machine = state.mode_machine;
  frame.enabled = true;
  frame.damping = true;
  return frame;
}

}  // namespace

ControlCore::ControlCore(SafetyLimits limits) : limits_(limits) {}

GatewayMode ControlCore::mode() const noexcept { return mode_; }

std::uint64_t ControlCore::generation() const noexcept { return generation_; }

std::uint64_t ControlCore::accepted_revision() const noexcept {
  return accepted_revision_;
}

void ControlCore::set_ready() {
  mode_ = GatewayMode::kReady;
  generation_ = 0;
  accepted_revision_ = 0;
  has_target_ = false;
  fault_code_.clear();
}

bool ControlCore::begin_high_level() {
  if (mode_ != GatewayMode::kReady) {
    return false;
  }
  mode_ = GatewayMode::kHighLevel;
  return true;
}

void ControlCore::finish_high_level() {
  if (mode_ == GatewayMode::kHighLevel) {
    mode_ = GatewayMode::kReady;
  }
}

bool ControlCore::arm_low_level(std::uint64_t generation,
                                const LowState& state,
                                std::uint64_t now_ns) {
  if (mode_ != GatewayMode::kReady || generation == 0 ||
      !fresh_state(state, now_ns, limits_.state_timeout_ns)) {
    return false;
  }

  generation_ = generation;
  accepted_revision_ = 0;
  has_target_ = false;
  target_ = {};
  target_.generation = generation;
  target_.q = state.q;
  mode_ = GatewayMode::kLowLevelArmed;
  fault_code_.clear();
  return true;
}

bool ControlCore::activate_low_level() {
  if (mode_ != GatewayMode::kLowLevelArmed) {
    return false;
  }
  mode_ = GatewayMode::kLowLevelActive;
  return true;
}

TargetError ControlCore::accept_target(const LowTarget& target,
                                       std::uint64_t now_ns) {
  if (mode_ != GatewayMode::kLowLevelArmed &&
      mode_ != GatewayMode::kLowLevelActive) {
    return TargetError::kWrongMode;
  }
  if (target.generation != generation_) {
    return TargetError::kWrongGeneration;
  }
  if (target.revision <= accepted_revision_) {
    return TargetError::kStaleRevision;
  }
  if (target.valid_until_ns <= now_ns) {
    return TargetError::kExpired;
  }
  if (!finite_vector(target.q) || !finite_vector(target.dq) ||
      !finite_vector(target.kp) || !finite_vector(target.kd) ||
      !finite_vector(target.tau)) {
    return TargetError::kNonFinite;
  }
  if (!within_symmetric_limit(target.q, limits_.max_abs_position) ||
      !within_symmetric_limit(target.dq, limits_.max_abs_velocity) ||
      !within_unsigned_limit(target.kp, limits_.max_kp) ||
      !within_unsigned_limit(target.kd, limits_.max_kd) ||
      !within_symmetric_limit(target.tau, limits_.max_abs_torque)) {
    return TargetError::kOutOfRange;
  }

  target_ = target;
  accepted_revision_ = target.revision;
  has_target_ = true;
  return TargetError::kNone;
}

TickResult ControlCore::tick(const LowState& state, std::uint64_t now_ns) {
  TickResult result;
  if (mode_ == GatewayMode::kFaulted) {
    result.frame = damping_frame(state);
    result.faulted = true;
    result.fault_code = fault_code_;
    return result;
  }
  if (mode_ != GatewayMode::kLowLevelActive) {
    result.faulted = true;
    result.fault_code = "wrong_mode";
    return result;
  }
  if (!fresh_state(state, now_ns, limits_.state_timeout_ns)) {
    fault("state_timeout");
    result.frame = damping_frame(state);
    result.faulted = true;
    result.fault_code = fault_code_;
    return result;
  }
  if (!has_target_) {
    fault("target_missing");
    result.frame = damping_frame(state);
    result.faulted = true;
    result.fault_code = fault_code_;
    return result;
  }
  if (target_.valid_until_ns <= now_ns) {
    fault("target_timeout");
    result.frame = damping_frame(state);
    result.faulted = true;
    result.fault_code = fault_code_;
    return result;
  }

  result.frame.q = target_.q;
  result.frame.dq = target_.dq;
  result.frame.kp = target_.kp;
  result.frame.kd = target_.kd;
  result.frame.tau = target_.tau;
  result.frame.mode_machine = state.mode_machine;
  result.frame.enabled = true;
  return result;
}

void ControlCore::stop_low_level() {
  if (mode_ == GatewayMode::kLowLevelArmed ||
      mode_ == GatewayMode::kLowLevelActive ||
      mode_ == GatewayMode::kFaulted) {
    set_ready();
  }
}

void ControlCore::fault(std::string code) {
  mode_ = GatewayMode::kFaulted;
  fault_code_ = std::move(code);
}

const char* to_string(GatewayMode mode) noexcept {
  switch (mode) {
    case GatewayMode::kOffline:
      return "offline";
    case GatewayMode::kReady:
      return "ready";
    case GatewayMode::kHighLevel:
      return "high_level";
    case GatewayMode::kLowLevelArmed:
      return "low_level_armed";
    case GatewayMode::kLowLevelActive:
      return "low_level_active";
    case GatewayMode::kFaulted:
      return "faulted";
  }
  return "unknown";
}

const char* to_string(TargetError error) noexcept {
  switch (error) {
    case TargetError::kNone:
      return "none";
    case TargetError::kWrongMode:
      return "wrong_mode";
    case TargetError::kWrongGeneration:
      return "wrong_generation";
    case TargetError::kStaleRevision:
      return "stale_revision";
    case TargetError::kExpired:
      return "expired";
    case TargetError::kNonFinite:
      return "non_finite";
    case TargetError::kOutOfRange:
      return "out_of_range";
  }
  return "unknown";
}

}  // namespace pg_unitree_r1
