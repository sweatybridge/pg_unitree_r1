#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pg_unitree_r1 {

inline constexpr std::size_t kMotorCount = 26;
using JointVector = std::array<float, kMotorCount>;

enum class GatewayMode {
  kOffline,
  kReady,
  kHighLevel,
  kLowLevelArmed,
  kLowLevelActive,
  kFaulted,
};

enum class TargetError {
  kNone,
  kWrongMode,
  kWrongGeneration,
  kStaleRevision,
  kExpired,
  kNonFinite,
  kOutOfRange,
};

struct SafetyLimits {
  float max_abs_position = 12.6F;
  float max_abs_velocity = 30.0F;
  float max_kp = 500.0F;
  float max_kd = 50.0F;
  float max_abs_torque = 120.0F;
  std::uint64_t state_timeout_ns = 100'000'000;
};

struct LowState {
  JointVector q{};
  JointVector dq{};
  std::uint8_t mode_machine = 0;
  std::uint64_t received_at_ns = 0;
  bool crc_valid = false;
};

struct LowTarget {
  JointVector q{};
  JointVector dq{};
  JointVector kp{};
  JointVector kd{};
  JointVector tau{};
  std::uint64_t generation = 0;
  std::uint64_t revision = 0;
  std::uint64_t valid_until_ns = 0;
};

struct ControlFrame {
  JointVector q{};
  JointVector dq{};
  JointVector kp{};
  JointVector kd{};
  JointVector tau{};
  std::uint8_t mode_machine = 0;
  bool enabled = false;
  bool damping = false;
};

struct TickResult {
  ControlFrame frame{};
  bool faulted = false;
  std::string fault_code;
};

class ControlCore {
 public:
  explicit ControlCore(SafetyLimits limits = {});

  GatewayMode mode() const noexcept;
  std::uint64_t generation() const noexcept;
  std::uint64_t accepted_revision() const noexcept;

  void set_ready();
  bool begin_high_level();
  void finish_high_level();

  bool arm_low_level(std::uint64_t generation, const LowState& state,
                     std::uint64_t now_ns);
  bool activate_low_level();
  TargetError accept_target(const LowTarget& target, std::uint64_t now_ns);
  TickResult tick(const LowState& state, std::uint64_t now_ns);
  void stop_low_level();
  void fault(std::string code);

 private:
  SafetyLimits limits_;
  GatewayMode mode_ = GatewayMode::kOffline;
  std::uint64_t generation_ = 0;
  std::uint64_t accepted_revision_ = 0;
  LowTarget target_{};
  bool has_target_ = false;
  std::string fault_code_;
};

const char* to_string(GatewayMode mode) noexcept;
const char* to_string(TargetError error) noexcept;

}  // namespace pg_unitree_r1
