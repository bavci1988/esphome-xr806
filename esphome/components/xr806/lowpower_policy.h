#pragma once

#include <cstdint>
#include <atomic>

namespace esphome::xr806 {

class XR806LowPowerPolicy {
 public:
  enum Blocker : uint32_t {
    UART_BUSY = 1u << 0,
    NETWORK_BUSY = 1u << 1,
    OTA_BUSY = 1u << 2,
    MCU_UNSYNCED = 1u << 3,
    WAKE_NOT_READY = 1u << 4,
    SCHEDULER_BUSY = 1u << 5,
    NOT_REQUESTED = 1u << 6,
    API_BUSY = 1u << 7,
    MDNS_BUSY = 1u << 8,
  };

  static constexpr uint32_t ALL_REQUIREMENTS =
      UART_BUSY | NETWORK_BUSY | OTA_BUSY | MCU_UNSYNCED | WAKE_NOT_READY | SCHEDULER_BUSY | API_BUSY | MDNS_BUSY;

  // Fail closed: initially all subsystems block standby.
  // This policy never enters standby by itself.
  void request_standby(bool requested) {
    if (commit_active_.load(std::memory_order_acquire)) {
      commit_abort_epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    if (requested_.exchange(requested, std::memory_order_acq_rel) != requested)
      generation_.fetch_add(1, std::memory_order_acq_rel);
  }

  bool standby_requested() const { return requested_.load(std::memory_order_acquire); }

  // Changes in owner votes invalidate an in-progress quiet interval.
  uint32_t generation() const { return generation_.load(std::memory_order_acquire); }

  void set_uart_idle(bool ready) { update_(UART_BUSY, ready); }

  void set_network_idle(bool ready) { update_(NETWORK_BUSY, ready); }

  // A registered WLAN owner may explicitly park the link for this PM
  // transaction. Tuya can postpone CMD02 network-state broadcasts until
  // resume, but the platform never interprets any Tuya commands.
  void set_network_quiescing(bool value) {
    if (network_quiescing_.exchange(value, std::memory_order_acq_rel) != value) {
      if (commit_active_.load(std::memory_order_acquire)) {
        commit_abort_epoch_.fetch_add(1, std::memory_order_acq_rel);
      }
      generation_.fetch_add(1, std::memory_order_acq_rel);
    }
  }
  bool network_quiescing() const { return network_quiescing_.load(std::memory_order_acquire); }

  void set_ota_idle(bool ready) { update_(OTA_BUSY, ready); }

  // No external MCU is required on a standalone WXU.
  // A separate, optional MCU driver must explicitly request this gate.
  // Each transition from absent to present resets the synchronization gate.
  void require_external_mcu(bool required) {
    if (commit_active_.load(std::memory_order_acquire)) {
      commit_abort_epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    if (required) {
      // Set the blocker BEFORE publishing the external-MCU requirement.
      // This avoids a transient unblocked window for a concurrent sleep voter.
      if (!external_mcu_required_.load(std::memory_order_acquire)) {
        blockers_.fetch_or(MCU_UNSYNCED, std::memory_order_acq_rel);
        external_mcu_required_.store(true, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
      }
    } else {
      if (external_mcu_required_.exchange(false, std::memory_order_acq_rel))
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }
  }

  void set_mcu_synchronized(bool ready) { update_(MCU_UNSYNCED, ready); }

  void set_api_idle(bool ready) { update_(API_BUSY, ready); }

  void set_mdns_idle(bool ready) { update_(MDNS_BUSY, ready); }

  void set_wake_source_ready(bool ready) { update_(WAKE_NOT_READY, ready); }

  void set_scheduler_idle(bool ready) { update_(SCHEDULER_BUSY, ready); }

  uint32_t blockers() const {
    uint32_t blockers = blockers_.load(std::memory_order_acquire);
    // An optional MCU may be completely absent, or may speak any protocol.
    // No HC32L136/Tuya assumption is allowed in the platform policy.
    if (!external_mcu_required_.load(std::memory_order_acquire)) {
      blockers &= ~MCU_UNSYNCED;
    }
    return blockers | (requested_.load(std::memory_order_acquire) ? 0u : NOT_REQUESTED);
  }

  bool can_enter_standby() const { return blockers() == 0; }

  // A best-effort, cross-task software admission fence. A new owner
  // transition invalidates this claim. Hardware producers must ALSO take
  // their vendor SDK PM wake lock BEFORE starting asynchronous work.
  bool begin_commit(uint32_t observed_generation) {
    // Sample a monotonic veto counter BEFORE acquiring the commit claim.
    // Clearing a shared abort flag would lose a concurrent worker's veto.
    // Unlike owner-vote generation, this also detects repeated busy
    // assertions while a final admission claim is being evaluated.
    const uint32_t abort_before = commit_abort_epoch_.load(std::memory_order_acquire);
    bool expected = false;
    if (!commit_active_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
      return false;
    commit_start_abort_epoch_.store(abort_before, std::memory_order_release);
    if (!this->commit_valid(observed_generation)) {
      this->end_commit();
      return false;
    }
    return true;
  }

  bool commit_valid(uint32_t observed_generation) const {
    return commit_active_.load(std::memory_order_acquire) &&
           commit_abort_epoch_.load(std::memory_order_acquire) ==
               commit_start_abort_epoch_.load(std::memory_order_acquire) &&
           generation_.load(std::memory_order_acquire) == observed_generation && blockers() == 0;
  }

  void end_commit() { commit_active_.store(false, std::memory_order_release); }

 private:
  void update_(uint32_t blocker, bool ready) {
    // Preempt an outstanding admission claim BEFORE changing an owner vote.
    if (commit_active_.load(std::memory_order_acquire)) {
      commit_abort_epoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    const uint32_t previous = ready ? blockers_.fetch_and(~blocker, std::memory_order_acq_rel)
                                    : blockers_.fetch_or(blocker, std::memory_order_acq_rel);
    if (((previous & blocker) == 0u) != ready)
      generation_.fetch_add(1, std::memory_order_acq_rel);
  }

  std::atomic<uint32_t> blockers_{ALL_REQUIREMENTS};
  std::atomic<bool> requested_{false};
  std::atomic<bool> external_mcu_required_{false};
  std::atomic<bool> network_quiescing_{false};
  std::atomic<uint32_t> generation_{0};
  std::atomic<bool> commit_active_{false};
  std::atomic<uint32_t> commit_abort_epoch_{0};
  std::atomic<uint32_t> commit_start_abort_epoch_{0};
};

#ifdef USE_XR806_LOWPOWER
// Shared instance, owned by the XR806 runtime.
// Callers must synchronize cross-task access.
XR806LowPowerPolicy &lowpower_policy();
#endif

}  // namespace esphome::xr806
