#include "ota_backend.h"
#ifdef USE_XR806_STANDBY_CONTROLLER
#include "esphome/components/xr806/lowpower_policy.h"
extern "C" {
#include "pm/pm.h"
}
#endif

#ifdef ESPHOME_THREAD_MULTI_ATOMICS
#include <atomic>
#endif

namespace esphome::ota {

bool version_is_older(const char *candidate, const char *reference) {
  if (candidate == nullptr || reference == nullptr)
    return false;
  while (true) {
    uint32_t a = 0;
    while (*candidate >= '0' && *candidate <= '9') {
      a = a * 10 + static_cast<uint32_t>(*candidate - '0');
      candidate++;
    }
    uint32_t b = 0;
    while (*reference >= '0' && *reference <= '9') {
      b = b * 10 + static_cast<uint32_t>(*reference - '0');
      reference++;
    }
    if (a != b)
      return a < b;
    // Components equal so far; advance past a single separator on each side.
    const bool a_more = (*candidate == '.');
    const bool b_more = (*reference == '.');
    if (a_more)
      candidate++;
    if (b_more)
      reference++;
    if (!a_more && !b_more)
      return false;  // Both strings exhausted with all components equal.
  }
}

// Non-blocking; may be released by another task than the one that took it.
#ifdef ESPHOME_THREAD_MULTI_ATOMICS
// uint32_t: narrower atomics compile to a masked CAS on Xtensa.
static std::atomic<uint32_t> update_locked{0};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

static bool update_lock_test_and_set() { return update_locked.exchange(1) == 0; }
static void update_lock_clear() { update_locked.store(0); }
#else
static bool update_locked = false;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

#ifdef ESPHOME_THREAD_MULTI_NO_ATOMICS
// BK72xx has no atomic exchange.
static Mutex &update_lock_mutex() {
  static Mutex mutex;
  return mutex;
}
#endif

static bool update_lock_test_and_set() {
#ifdef ESPHOME_THREAD_MULTI_NO_ATOMICS
  LockGuard guard(update_lock_mutex());
#else
  // lwIP callbacks may run from an interrupt (RP2040).
  InterruptLock guard;
#endif
  if (update_locked)
    return false;
  update_locked = true;
  return true;
}

static void update_lock_clear() {
#ifdef ESPHOME_THREAD_MULTI_NO_ATOMICS
  LockGuard guard(update_lock_mutex());
#endif
  update_locked = false;
}
#endif

#ifdef USE_XR806_STANDBY_CONTROLLER
static struct wakelock ota_update_pm_wakelock{};  // NOLINT
static bool ota_update_pm_wakelock_held = false;  // Single global UpdateLock owner.

static bool ota_update_pm_lock_acquire() {
  if (ota_update_pm_wakelock_held) {
    return true;
  }
  ota_update_pm_wakelock.name = const_cast<char *>("esphome.xr806.ota-update");
  if (pm_wake_lock(&ota_update_pm_wakelock) != 0) {
    return false;
  }
  ota_update_pm_wakelock_held = true;
  return true;
}

static void ota_update_pm_lock_release() {
  if (!ota_update_pm_wakelock_held) {
    return;
  }
  if (pm_wake_unlock(&ota_update_pm_wakelock) == 0) {
    ota_update_pm_wakelock_held = false;
  } else {
    xr806::lowpower_policy().set_ota_idle(false);  // Sticky fail closed.
  }
}
#endif

bool global_update_active() {
#ifdef ESPHOME_THREAD_MULTI_ATOMICS
  return update_locked.load(std::memory_order_acquire) != 0;
#else
  return update_locked;
#endif
}

bool UpdateLock::try_acquire() {
  if (!this->held_) {
    this->held_ = update_lock_test_and_set();
#ifdef USE_XR806_STANDBY_CONTROLLER
    if (this->held_) {
      xr806::lowpower_policy().set_ota_idle(false);  // Publish veto BEFORE any flash work.
      if (!ota_update_pm_lock_acquire()) {
        this->held_ = false;
        update_lock_clear();
        return false;
      }
    }
#endif
  }
  return this->held_;
}

void UpdateLock::release() {
  if (!this->held_) {
    return;
  }
#ifdef USE_XR806_STANDBY_CONTROLLER
  ota_update_pm_lock_release();
#endif
  update_lock_clear();
  this->held_ = false;
}

void UpdateLock::force_release() {
#ifdef USE_XR806_STANDBY_CONTROLLER
  ota_update_pm_lock_release();
#endif
  update_lock_clear();
}

#ifdef USE_OTA_STATE_LISTENER
OTAGlobalCallback *global_ota_callback{nullptr};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

OTAGlobalCallback *get_global_ota_callback() {
  if (global_ota_callback == nullptr) {
    global_ota_callback = new OTAGlobalCallback();  // NOLINT(cppcoreguidelines-owning-memory)
  }
  return global_ota_callback;
}

void OTAComponent::notify_state_deferred_(OTAState state, float progress, uint8_t error) {
  // Pack state, error, and progress into a single uint32_t so the lambda
  // captures only [this, packed] (8 bytes) — fits in std::function SBO.
  // Layout: [state:8][error:8][progress_fixed:16] where progress is 0–10000 (0.01% resolution)
  static_assert(OTA_ERROR <= 0xFF, "OTAState must fit in 8 bits for packing");
  uint32_t packed = (static_cast<uint32_t>(state) << 24) | (static_cast<uint32_t>(error) << 16) |
                    static_cast<uint16_t>(progress * 100.0f);
  this->defer([this, packed]() {
    this->notify_state_(static_cast<OTAState>(packed >> 24), static_cast<float>(packed & 0xFFFF) / 100.0f,
                        static_cast<uint8_t>(packed >> 16));
  });
}

void OTAComponent::notify_state_(OTAState state, float progress, uint8_t error) {
  for (auto *listener : this->state_listeners_) {
    listener->on_ota_state(state, progress, error);
  }
  get_global_ota_callback()->notify_ota_state(state, progress, error, this);
}
#endif

}  // namespace esphome::ota
