#pragma once

// Portable lifecycle state: service quiescence is independent of the UART
// protocol and MUST complete before the PM controller releases owner votes.
#include <cstdint>

namespace esphome::xr806 {

class XR806StandbyServiceLifecycle {
 public:
  enum class Phase : uint8_t { ACTIVE, PREPARING, QUIESCED, ENTERING, RESUMING };
  static constexpr uint32_t PREPARE_TIMEOUT_MS = 2000;

  bool begin(uint32_t now) {
    if (phase_ != Phase::ACTIVE) {
      return false;
    }
    phase_ = Phase::PREPARING;
    prepared_at_ = now;
    ++generation_;
    return true;
  }

  bool preparing() const { return phase_ == Phase::PREPARING; }
  bool quiesced() const { return phase_ == Phase::QUIESCED; }
  bool paused() const { return phase_ == Phase::QUIESCED || phase_ == Phase::ENTERING; }
  bool active() const { return phase_ == Phase::ACTIVE; }
  Phase phase() const { return phase_; }
  uint32_t generation() const { return generation_; }

  bool expired(uint32_t now) const {
    return preparing() && static_cast<uint32_t>(now - prepared_at_) >= PREPARE_TIMEOUT_MS;
  }

  bool ready(bool all_services_quiesced) {
    if (!preparing() || !all_services_quiesced) {
      return false;
    }
    phase_ = Phase::QUIESCED;
    ++generation_;
    return true;
  }

  bool enter() {
    if (!quiesced()) {
      return false;
    }
    phase_ = Phase::ENTERING;
    ++generation_;
    return true;
  }

  void resume() {
    phase_ = Phase::RESUMING;
    ++generation_;
  }

  void restored() {
    phase_ = Phase::ACTIVE;
    ++generation_;
  }

 private:
  Phase phase_{Phase::ACTIVE};
  uint32_t prepared_at_{0};
  uint32_t generation_{0};
};

}  // namespace esphome::xr806
