#pragma once

// Atomic request+epoch for asynchronous service suspend acknowledgements.
// Never authorizes PM by itself: owners still have to close/drain their
// hardware resources, publish the PM idle vote and recheck the same token.
#include <atomic>
#include <cstdint>

namespace esphome::xr806 {

class XR806StandbyAsyncEpoch {
 public:
  bool request(bool suspend) {
    // Encode the request bit and generation in ONE atomic word. Separate
    // exchange(bool) + increment(epoch) exposes a transient ABA window
    // where an old lwIP callback can see a new request with an old epoch.
    uint32_t previous = state_.load(std::memory_order_acquire);
    while (true) {
      if (((previous & 1u) != 0u) == suspend) {
        return false;
      }
      const uint32_t next = ((previous + 2u) & ~1u) | (suspend ? 1u : 0u);
      if (state_.compare_exchange_weak(previous, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
      }
    }
  }

  bool requested() const { return (state_.load(std::memory_order_acquire) & 1u) != 0u; }

  // A token includes both its generation and the requested bit. The actual
  // numeric generation is intentionally not exposed as a time or counter.
  uint32_t epoch() const { return state_.load(std::memory_order_acquire); }

  bool current(uint32_t token) const { return (token & 1u) != 0u && state_.load(std::memory_order_acquire) == token; }

 private:
  std::atomic<uint32_t> state_{0};
};

}  // namespace esphome::xr806
