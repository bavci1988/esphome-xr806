#pragma once

// Model-only OTA stream state machine. No flash callback or vendor SDK calls
// exist here. It simulates boundaries and crash/timeout behavior before the
// eventual production backend is allowed to link.
#include <cstdint>
#include "ota_vendor_runtime_preflight.h"

namespace esphome::xr806 {

class XR806OTADryRunSession {
 public:
  enum class Phase : uint8_t { IDLE, RECEIVING, VERIFYING, COMPLETE, ABORTED, FAILED };
  enum class Failure : uint8_t {
    NONE,
    NOT_ADMITTED,
    BAD_PHASE,
    INVALID_CHUNK,
    CAPACITY,
    TIMEOUT,
    IMAGE_VERIFY,
    POWER_LOSS,
  };
  static constexpr uint32_t MAX_CHUNK = 8192;
  static constexpr uint32_t TRANSFER_TIMEOUT_MS = 105000;

  bool begin(const XR806OTARuntimeResult &plan, uint32_t intended_bytes, uint32_t now) {
    if (phase_ != Phase::IDLE)
      return fail_(Failure::BAD_PHASE);
    if (plan.error != XR806OTARuntimeError::OK || intended_bytes == 0 || intended_bytes > plan.candidate.length)
      return fail_(Failure::NOT_ADMITTED);
    expected_bytes_ = intended_bytes;
    total_bytes_ = 0;
    last_activity_ms_ = now;
    phase_ = Phase::RECEIVING;
    return true;
  }

  bool receive(uint32_t bytes, uint32_t now) {
    if (phase_ != Phase::RECEIVING)
      return fail_(Failure::BAD_PHASE);
    if (expired(now))
      return fail_(Failure::TIMEOUT);
    if (bytes == 0 || bytes > MAX_CHUNK)
      return fail_(Failure::INVALID_CHUNK);
    if (bytes > expected_bytes_ - total_bytes_)
      return fail_(Failure::CAPACITY);
    total_bytes_ += bytes;
    last_activity_ms_ = now;
    return true;
  }

  bool finalize(bool image_verified, uint32_t now) {
    if (phase_ != Phase::RECEIVING || expected_bytes_ != total_bytes_)
      return fail_(Failure::BAD_PHASE);
    if (expired(now))
      return fail_(Failure::TIMEOUT);
    phase_ = Phase::VERIFYING;
    if (!image_verified)
      return fail_(Failure::IMAGE_VERIFY);
    phase_ = Phase::COMPLETE;
    return true;
  }

  bool expired(uint32_t now) const {
    return phase_ == Phase::RECEIVING && static_cast<uint32_t>(now - last_activity_ms_) >= TRANSFER_TIMEOUT_MS;
  }

  void abort() {
    if (phase_ != Phase::COMPLETE && phase_ != Phase::FAILED)
      phase_ = Phase::ABORTED;
  }

  void power_loss() {
    if (phase_ != Phase::COMPLETE) {
      phase_ = Phase::FAILED;
      failure_ = Failure::POWER_LOSS;
    }
    // Never infer resume capability from previously received bytes.
    total_bytes_ = 0;
  }

  Phase phase() const { return phase_; }
  Failure failure() const { return failure_; }
  uint32_t total_bytes() const { return total_bytes_; }
  uint32_t expected_bytes() const { return expected_bytes_; }
  bool may_publish_boot_selection() const { return false; }  // Never in a simulation.

 private:
  bool fail_(Failure reason) {
    phase_ = Phase::FAILED;
    failure_ = reason;
    return false;
  }
  Phase phase_{Phase::IDLE};
  Failure failure_{Failure::NONE};
  uint32_t expected_bytes_{0};
  uint32_t total_bytes_{0};
  uint32_t last_activity_ms_{0};
};

}  // namespace esphome::xr806
