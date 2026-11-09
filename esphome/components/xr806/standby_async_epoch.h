#pragma once

// Monotonic generation for asynchronous service suspend acknowledgements.
// Does not itself release PM votes or claim a socket/driver is drained.
// The service must check the generation around its own close + idle vote.
#include <atomic>
#include <cstdint>

namespace esphome::xr806 {

class XR806StandbyAsyncEpoch {
 public:
  bool request(bool suspend) {
    // Called by the ESPHome owner; the lwIP/driver task only observes it.
    if (requested_.exchange(suspend, std::memory_order_acq_rel) == suspend) {
      return false;
    }
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    return true;
  }

  bool requested() const { return requested_.load(std::memory_order_acquire); }
  uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }

  // A suspend result from a previous request must never authorize PM.
  bool current(uint32_t observed_epoch) const { return this->requested() && this->epoch() == observed_epoch; }

 private:
  std::atomic<bool> requested_{false};
  std::atomic<uint32_t> epoch_{0};
};

}  // namespace esphome::xr806
