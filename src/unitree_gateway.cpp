#include "pg_unitree_r1/unitree_gateway.hpp"

#include <unitree/idl/hg/LowCmd_.hpp>
#include <unitree/idl/hg/LowState_.hpp>
#include <unitree/robot/b2/motion_switcher/motion_switcher_client.hpp>
#include <unitree/robot/channel/channel_publisher.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>
#include <unitree/robot/r1/loco/r1_loco_client.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace pg_unitree_r1 {
namespace {

using DdsLowCmd = unitree_hg::msg::dds_::LowCmd_;
using DdsLowState = unitree_hg::msg::dds_::LowState_;

constexpr char kLowCommandTopic[] = "rt/lowcmd";
constexpr char kLowStateTopic[] = "rt/lowstate";
constexpr std::chrono::microseconds kControlPeriod{2000};
constexpr std::array<std::size_t, kMotorCount> kJointIndexInIdl = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
    13, 15, 16, 17, 18, 19, 22, 23, 24, 25, 26, 29, 30};

std::uint32_t crc32_core(std::uint32_t* data, std::uint32_t words) {
  std::uint32_t crc = 0xFFFFFFFFU;
  constexpr std::uint32_t polynomial = 0x04C11DB7U;
  for (std::uint32_t i = 0; i < words; ++i) {
    std::uint32_t bit = 1U << 31U;
    for (std::uint32_t n = 0; n < 32U; ++n) {
      if ((crc & 0x80000000U) != 0U) {
        crc = (crc << 1U) ^ polynomial;
      } else {
        crc <<= 1U;
      }
      if ((data[i] & bit) != 0U) {
        crc ^= polynomial;
      }
      bit >>= 1U;
    }
  }
  return crc;
}

ControlFrame make_damping_frame(const LowState& state) {
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

GatewayResult success(int sdk_code = 0, std::string detail = {}) {
  return {true, sdk_code, {}, std::move(detail)};
}

GatewayResult failure(std::string code, std::string detail,
                      int sdk_code = 0) {
  return {false, sdk_code, std::move(code), std::move(detail)};
}

class LowLevelAdapter {
 public:
  LowLevelAdapter() = default;
  ~LowLevelAdapter() { stop(); }

  void initialize_channels() {
    publisher_ = std::make_shared<unitree::robot::ChannelPublisher<DdsLowCmd>>(
        kLowCommandTopic);
    publisher_->InitChannel();
    subscriber_ =
        std::make_shared<unitree::robot::ChannelSubscriber<DdsLowState>>(
            kLowStateTopic);
    subscriber_->InitChannel(
        std::bind(&LowLevelAdapter::on_state, this, std::placeholders::_1), 1);
  }

  bool begin_high_level() {
    std::lock_guard<std::mutex> lock(core_mutex_);
    return core_.begin_high_level();
  }

  void finish_high_level() {
    std::lock_guard<std::mutex> lock(core_mutex_);
    core_.finish_high_level();
  }

  void set_ready() {
    std::lock_guard<std::mutex> lock(core_mutex_);
    core_.set_ready();
  }

  GatewayResult prepare(std::uint64_t generation,
                        std::uint32_t state_timeout_ms,
                        std::uint32_t damping_ms) {
    if (running_.load(std::memory_order_acquire)) {
      return failure("low_level_active", "the low-level loop is already running");
    }
    damping_ns_ = static_cast<std::uint64_t>(damping_ms) * 1'000'000ULL;

    LowState state;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      if (copy_state(state)) {
        SafetyLimits limits;
        limits.state_timeout_ns =
            static_cast<std::uint64_t>(state_timeout_ms) * 1'000'000ULL;
        std::lock_guard<std::mutex> lock(core_mutex_);
        core_ = ControlCore(limits);
        core_.set_ready();
        if (core_.arm_low_level(generation, state, monotonic_now_ns())) {
          clear_fault();
          return success();
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return failure("state_unavailable",
                   "no fresh CRC-valid rt/lowstate sample arrived within 5 seconds");
  }

  GatewayResult update_target(const LowTarget& target) {
    std::lock_guard<std::mutex> lock(core_mutex_);
    const auto error = core_.accept_target(target, monotonic_now_ns());
    if (error != TargetError::kNone) {
      return failure(to_string(error), "low-level target rejected by safety core");
    }
    return success();
  }

  GatewayResult start() {
    if (running_.load(std::memory_order_acquire)) {
      return failure("low_level_active", "the low-level loop is already running");
    }
    if (control_thread_.joinable()) {
      control_thread_.join();
    }
    {
      std::lock_guard<std::mutex> lock(core_mutex_);
      if (!core_.activate_low_level()) {
        return failure("wrong_mode", "the low-level session is not armed");
      }
    }
    stop_requested_.store(false, std::memory_order_release);
    stop_requested_at_ns_.store(0, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    control_thread_ = std::thread(&LowLevelAdapter::control_loop, this);
    return success();
  }

  GatewayResult stop() {
    if (running_.load(std::memory_order_acquire)) {
      stop_requested_at_ns_.store(monotonic_now_ns(), std::memory_order_release);
      stop_requested_.store(true, std::memory_order_release);
    }
    if (control_thread_.joinable()) {
      control_thread_.join();
    }
    {
      std::lock_guard<std::mutex> lock(core_mutex_);
      core_.stop_low_level();
    }
    return success();
  }

  GatewaySnapshot snapshot() const {
    GatewaySnapshot snapshot;
    {
      std::lock_guard<std::mutex> lock(core_mutex_);
      snapshot.mode = core_.mode();
      snapshot.active_generation = core_.generation();
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      snapshot.latest_state = latest_state_;
      snapshot.has_state = has_state_;
      snapshot.robot_tick = robot_tick_;
      snapshot.state_received_at_ns = latest_state_.received_at_ns;
    }
    snapshot.control_ticks = control_ticks_.load(std::memory_order_relaxed);
    snapshot.deadline_misses = deadline_misses_.load(std::memory_order_relaxed);
    snapshot.crc_errors = crc_errors_.load(std::memory_order_relaxed);
    snapshot.publish_failures =
        publish_failures_.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(fault_mutex_);
      snapshot.fault_code = fault_code_;
    }
    return snapshot;
  }

 private:
  bool copy_state(LowState& state) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!has_state_) {
      return false;
    }
    state = latest_state_;
    return true;
  }

  void clear_fault() {
    std::lock_guard<std::mutex> lock(fault_mutex_);
    fault_code_.clear();
  }

  void latch_fault(const std::string& code) {
    std::lock_guard<std::mutex> lock(fault_mutex_);
    if (fault_code_.empty()) {
      fault_code_ = code;
    }
  }

  void on_state(const void* message) {
    DdsLowState dds_state = *static_cast<const DdsLowState*>(message);
    const auto expected = crc32_core(
        reinterpret_cast<std::uint32_t*>(&dds_state),
        static_cast<std::uint32_t>((sizeof(DdsLowState) >> 2U) - 1U));
    if (dds_state.crc() != expected) {
      crc_errors_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    LowState state;
    state.crc_valid = true;
    state.mode_machine = dds_state.mode_machine();
    state.received_at_ns = monotonic_now_ns();
    for (std::size_t i = 0; i < kMotorCount; ++i) {
      const auto idl_index = kJointIndexInIdl[i];
      state.q[i] = dds_state.motor_state()[idl_index].q();
      state.dq[i] = dds_state.motor_state()[idl_index].dq();
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_state_ = state;
    robot_tick_ = dds_state.tick();
    has_state_ = true;
  }

  void publish(const ControlFrame& frame) {
    if (!frame.enabled || !publisher_) {
      return;
    }
    DdsLowCmd command;
    command.mode_pr() = 0;
    command.mode_machine() = frame.mode_machine;
    for (std::size_t i = 0; i < kMotorCount; ++i) {
      auto& motor = command.motor_cmd()[kJointIndexInIdl[i]];
      motor.mode() = 1;
      motor.q() = frame.q[i];
      motor.dq() = frame.dq[i];
      motor.kp() = frame.kp[i];
      motor.kd() = frame.kd[i];
      motor.tau() = frame.tau[i];
    }
    command.crc() = crc32_core(
        reinterpret_cast<std::uint32_t*>(&command),
        static_cast<std::uint32_t>((sizeof(DdsLowCmd) >> 2U) - 1U));
    if (!publisher_->Write(command)) {
      publish_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void control_loop() {
    auto next = std::chrono::steady_clock::now();
    std::uint64_t faulted_at_ns = 0;
    while (true) {
      const auto now_ns = monotonic_now_ns();
      LowState state;
      if (!copy_state(state)) {
        state.received_at_ns = 0;
      }

      ControlFrame frame;
      bool faulted = false;
      std::string fault_code;
      const auto stop_requested =
          stop_requested_.load(std::memory_order_acquire);
      if (stop_requested) {
        frame = make_damping_frame(state);
        const auto requested_at =
            stop_requested_at_ns_.load(std::memory_order_acquire);
        if (requested_at != 0 && now_ns - requested_at >= damping_ns_) {
          break;
        }
      } else if (core_mutex_.try_lock()) {
        const auto result = core_.tick(state, now_ns);
        core_mutex_.unlock();
        frame = result.frame;
        faulted = result.faulted;
        fault_code = result.fault_code;
      } else {
        deadline_misses_.fetch_add(1, std::memory_order_relaxed);
      }

      if (faulted) {
        latch_fault(fault_code);
        if (faulted_at_ns == 0) {
          faulted_at_ns = now_ns;
        } else if (now_ns - faulted_at_ns >= damping_ns_) {
          break;
        }
      }
      publish(frame);
      control_ticks_.fetch_add(1, std::memory_order_relaxed);

      next += kControlPeriod;
      const auto after = std::chrono::steady_clock::now();
      if (after > next) {
        deadline_misses_.fetch_add(1, std::memory_order_relaxed);
        next = after + kControlPeriod;
      }
      std::this_thread::sleep_until(next);
    }
    running_.store(false, std::memory_order_release);
  }

  mutable std::mutex core_mutex_;
  ControlCore core_{};
  mutable std::mutex state_mutex_;
  LowState latest_state_{};
  bool has_state_ = false;
  std::uint64_t robot_tick_ = 0;
  mutable std::mutex fault_mutex_;
  std::string fault_code_;
  std::shared_ptr<unitree::robot::ChannelPublisher<DdsLowCmd>> publisher_;
  std::shared_ptr<unitree::robot::ChannelSubscriber<DdsLowState>> subscriber_;
  std::thread control_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::uint64_t> stop_requested_at_ns_{0};
  std::uint64_t damping_ns_ = 250'000'000ULL;
  std::atomic<std::uint64_t> control_ticks_{0};
  std::atomic<std::uint64_t> deadline_misses_{0};
  std::atomic<std::uint64_t> crc_errors_{0};
  std::atomic<std::uint64_t> publish_failures_{0};
};

}  // namespace

std::uint64_t monotonic_now_ns() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

class UnitreeGateway::Impl {
 public:
  GatewayResult initialize(const std::string& network_interface,
                           float sdk_timeout_s) {
    try {
      unitree::robot::ChannelFactory::Instance()->Init(0, network_interface);
      loco_ = std::make_unique<unitree::robot::r1::LocoClient>();
      loco_->SetTimeout(sdk_timeout_s);
      loco_->Init();
      motion_switcher_ =
          std::make_unique<unitree::robot::b2::MotionSwitcherClient>();
      motion_switcher_->SetTimeout(sdk_timeout_s);
      motion_switcher_->Init();
      low_.initialize_channels();
      {
        std::lock_guard<std::mutex> lock(initialization_mutex_);
        initialized_ = true;
      }
      low_.set_ready();
      return success();
    } catch (const std::exception& error) {
      return failure("sdk_initialization_failed", error.what());
    } catch (...) {
      return failure("sdk_initialization_failed", "unknown SDK exception");
    }
  }

  GatewayResult execute(HighCommand command, const HighCommandArgs& args) {
    if (!is_initialized()) {
      return failure("gateway_offline", "the Unitree gateway is not initialized");
    }
    if (!low_.begin_high_level()) {
      return failure("mode_conflict",
                     "high-level control is unavailable during a low-level session");
    }

    int code = 0;
    try {
      switch (command) {
        case HighCommand::kStart:
          code = loco_->Start();
          break;
        case HighCommand::kStandUp:
          code = loco_->StandUp();
          break;
        case HighCommand::kDamp:
          code = loco_->Damp();
          break;
        case HighCommand::kZeroTorque:
          code = loco_->ZeroTorque();
          break;
        case HighCommand::kStopMove:
          code = loco_->StopMove();
          break;
        case HighCommand::kMove:
          code = loco_->SetVelocity(args.vx, args.vy, args.yaw,
                                    args.duration_s);
          break;
        case HighCommand::kSetVelocity:
          code = loco_->SetVelocity(args.vx, args.vy, args.yaw,
                                    args.duration_s);
          break;
        case HighCommand::kSetSpeedMode:
          code = loco_->SetSpeedMode(args.integer_arg);
          break;
      }
    } catch (const std::exception& error) {
      low_.finish_high_level();
      return failure("sdk_exception", error.what());
    } catch (...) {
      low_.finish_high_level();
      return failure("sdk_exception", "unknown SDK exception");
    }
    low_.finish_high_level();
    if (code != 0) {
      return failure("sdk_error", "Unitree high-level RPC returned an error", code);
    }
    return success(code);
  }

  GatewayResult prepare_low_level(std::uint64_t generation,
                                  std::uint32_t state_timeout_ms,
                                  std::uint32_t damping_ms) {
    if (!is_initialized()) {
      return failure("gateway_offline", "the Unitree gateway is not initialized");
    }
    try {
      std::string form;
      std::string name;
      int code = motion_switcher_->CheckMode(form, name);
      if (code != 0) {
        return failure("motion_mode_check_failed",
                       "failed to query the current Unitree motion mode", code);
      }
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!name.empty() && std::chrono::steady_clock::now() < deadline) {
        code = motion_switcher_->ReleaseMode();
        if (code != 0) {
          return failure("motion_mode_release_failed",
                         "failed to release the Unitree motion service", code);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        code = motion_switcher_->CheckMode(form, name);
        if (code != 0) {
          return failure("motion_mode_check_failed",
                         "failed to confirm release of the Unitree motion service",
                         code);
        }
      }
      if (!name.empty()) {
        return failure("motion_mode_release_timeout",
                       "the Unitree motion service did not release within 5 seconds");
      }
      return low_.prepare(generation, state_timeout_ms, damping_ms);
    } catch (const std::exception& error) {
      return failure("sdk_exception", error.what());
    } catch (...) {
      return failure("sdk_exception", "unknown SDK exception");
    }
  }

  GatewayResult update_target(const LowTarget& target) {
    return low_.update_target(target);
  }

  GatewayResult start_low_level() { return low_.start(); }

  GatewayResult stop_low_level() { return low_.stop(); }

  GatewaySnapshot snapshot() const { return low_.snapshot(); }

 private:
  bool is_initialized() const {
    std::lock_guard<std::mutex> lock(initialization_mutex_);
    return initialized_;
  }

  mutable std::mutex initialization_mutex_;
  bool initialized_ = false;
  std::unique_ptr<unitree::robot::r1::LocoClient> loco_;
  std::unique_ptr<unitree::robot::b2::MotionSwitcherClient> motion_switcher_;
  LowLevelAdapter low_;
};

UnitreeGateway::UnitreeGateway() : impl_(std::make_unique<Impl>()) {}
UnitreeGateway::~UnitreeGateway() = default;

GatewayResult UnitreeGateway::initialize(const std::string& network_interface,
                                         float sdk_timeout_s) {
  return impl_->initialize(network_interface, sdk_timeout_s);
}

GatewayResult UnitreeGateway::execute(HighCommand command,
                                      const HighCommandArgs& args) {
  return impl_->execute(command, args);
}

GatewayResult UnitreeGateway::prepare_low_level(
    std::uint64_t generation, std::uint32_t state_timeout_ms,
    std::uint32_t damping_ms) {
  return impl_->prepare_low_level(generation, state_timeout_ms, damping_ms);
}

GatewayResult UnitreeGateway::update_target(const LowTarget& target) {
  return impl_->update_target(target);
}

GatewayResult UnitreeGateway::start_low_level() {
  return impl_->start_low_level();
}

GatewayResult UnitreeGateway::stop_low_level() {
  return impl_->stop_low_level();
}

GatewaySnapshot UnitreeGateway::snapshot() const { return impl_->snapshot(); }

}  // namespace pg_unitree_r1
