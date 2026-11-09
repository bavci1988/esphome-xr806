#pragma once

#include <cstdint>

// Portable life-cycle state, deliberately separate from Tuya CMD38.
// A service cannot claim "idle" merely because its TCP connection count
// happened to be zero: the listener, callbacks and driver must be parked.
namespace esphome::xr806 {

class XR806StandbyNetworkLifecycle {
 public:
  enum Owner : uint8_t { WLAN = 1u << 0, API = 1u << 1, OTA = 1u << 2, MDNS = 1u << 3 };
  enum class Phase : uint8_t { RUNNING, QUIESCING, READY, RESUMING };

  void configure(uint8_t present) {
    present_ = present;
    this->resume();
  }

  // Request permission to drain real service owners; this is not PM entry.
  void begin(uint32_t generation) {
    if (phase_ != Phase::RUNNING)
      return;
    phase_ = Phase::QUIESCING;
    quiesced_ = 0;
    generation_ = generation;
  }

  // Only the actual service owner may call this after close/drain/disable
  // has completed, never from an inferred "no network traffic" timer.
  bool owner_quiesced(Owner owner, uint32_t generation) {
    if (phase_ != Phase::QUIESCING && phase_ != Phase::READY)
      return false;
    if ((present_ & owner) == 0 || generation_ != generation)
      return false;
    quiesced_ |= owner;
    if ((quiesced_ & present_) == present_)
      phase_ = Phase::READY;
    return true;
  }

  // Every new ISR, socket accept, OTA write, callback or reconnect must
  // invalidate any stale readiness. Subsequent admission needs a new epoch.
  void activity(Owner owner) {
    if ((present_ & owner) == 0)
      return;
    ++generation_;
    quiesced_ &= ~owner;
    phase_ = Phase::RUNNING;
  }

  bool ready(uint32_t generation) const {
    return phase_ == Phase::READY && generation_ == generation && (quiesced_ & present_) == present_;
  }

  uint32_t generation() const { return generation_; }
  uint8_t pending() const { return present_ & ~quiesced_; }
  Phase phase() const { return phase_; }

  void resume() {
    ++generation_;
    quiesced_ = 0;
    phase_ = Phase::RUNNING;
  }

 private:
  uint32_t generation_{0};
  uint8_t present_{0};
  uint8_t quiesced_{0};
  Phase phase_{Phase::RUNNING};
};

}  // namespace esphome::xr806
