#include "esphome/core/defines.h"

#ifdef USE_XR806

#include "preferences.h"

#include <cinttypes>
#include <cstring>

#include "esphome/core/log.h"

extern "C" {
#include "demo/trv16_port/prj_config.h"
#include "image/fdcm.h"
#include "image/flash.h"
}

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.preferences");

// The only code that erases these sectors; keep the layout it relies on next to it.
static_assert(PRJCONF_ESPHOME_PREFS_FLASH == PRJCONF_SYSINFO_FLASH);
static_assert(PRJCONF_ESPHOME_PREFS_SECTOR == 4096);
static_assert(PRJCONF_ESPHOME_PREFS0_ADDR == 1972 * 1024);
static_assert(PRJCONF_ESPHOME_PREFS0_ADDR + PRJCONF_ESPHOME_PREFS_SECTOR == PRJCONF_ESPHOME_PREFS1_ADDR);
static_assert(PRJCONF_ESPHOME_PREFS1_ADDR + PRJCONF_ESPHOME_PREFS_SECTOR == PRJCONF_SYSINFO_ADDR);
// One snapshot per sector: every FDCM write erases the sector, so the other one always survives.
static_assert(2 * XR806Preferences::SNAPSHOT_SIZE > PRJCONF_ESPHOME_PREFS_SECTOR - 9);

static constexpr uint32_t FDCM_ID_CODE = 0xA55A5AA5;
static constexpr uint32_t SECTOR_ADDRESS[2] = {PRJCONF_ESPHOME_PREFS0_ADDR, PRJCONF_ESPHOME_PREFS1_ADDR};

namespace {

struct FdcmStorage {
  fdcm_handle_t *slots[2]{};

  bool read(uint8_t slot, uint8_t *data, size_t size) {
    return fdcm_read(this->slots[slot], data, static_cast<uint16_t>(size)) == size;
  }
  bool write(uint8_t slot, const uint8_t *data, size_t size) {
    return fdcm_write(this->slots[slot], data, static_cast<uint16_t>(size)) == size;
  }
  bool erase(uint8_t slot) { return fdcm_erase(this->slots[slot]) == 0; }
};

FdcmStorage fdcm_storage;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// FDCM erases a sector whose header does not match before it writes. Accept only a fully
// erased sector or one that FDCM already formatted for our snapshot size, so foreign data
// (for example from the original Tuya firmware) is never erased.
bool sector_is_ours_or_blank(uint32_t address) {
  uint8_t probe[64];
  uint32_t magic = 0;
  uint16_t data_size = 0;
  bool all_erased = true;
  for (uint32_t offset = 0; offset < PRJCONF_ESPHOME_PREFS_SECTOR; offset += sizeof(probe)) {
    if (flash_read(PRJCONF_ESPHOME_PREFS_FLASH, address + offset, probe, sizeof(probe)) != sizeof(probe))
      return false;
    if (offset == 0) {
      std::memcpy(&magic, probe, sizeof(magic));
      std::memcpy(&data_size, probe + 6, sizeof(data_size));  // fdcm_header_t.data_size
    }
    for (uint8_t value : probe) {
      if (value != 0xFF) {
        all_erased = false;
        break;
      }
    }
    if (!all_erased)
      break;  // The header was read with the first block.
  }
  return all_erased || (magic == FDCM_ID_CODE && data_size == XR806Preferences::SNAPSHOT_SIZE);
}

}  // namespace

bool XR806Preferences::open_() {
  if (this->initialized_)
    return this->available_;
  this->initialized_ = true;

  for (uint8_t slot = 0; slot < 2; slot++) {
    if (!sector_is_ours_or_blank(SECTOR_ADDRESS[slot])) {
      ESP_LOGE(TAG, "Foreign data in preference sector at 0x%06X; preferences are not saved",
               static_cast<unsigned>(SECTOR_ADDRESS[slot]));
      return false;
    }
    fdcm_storage.slots[slot] =
        fdcm_open(PRJCONF_ESPHOME_PREFS_FLASH, SECTOR_ADDRESS[slot], PRJCONF_ESPHOME_PREFS_SECTOR);
    if (fdcm_storage.slots[slot] == nullptr) {
      ESP_LOGE(TAG, "Cannot open preference sector at 0x%06X", static_cast<unsigned>(SECTOR_ADDRESS[slot]));
      return false;
    }
  }

  if (this->journal_.load(fdcm_storage)) {
    ESP_LOGD(TAG, "Loaded preferences from sector %u (generation %" PRIu32 ", %u bytes)", this->journal_.active_slot(),
             this->journal_.generation(), static_cast<unsigned>(this->journal_.used()));
  } else {
    ESP_LOGD(TAG, "No saved preferences");
  }
  this->available_ = true;
  return true;
}

ESPPreferenceObject XR806Preferences::make_preference(size_t length, uint32_t type) {
  if (length == 0 || length > SNAPSHOT_SIZE)
    return {};
  // Handles live until reboot, as on the other platforms.
  return ESPPreferenceObject(
      new XR806PreferenceBackend(this, type, length));  // NOLINT(cppcoreguidelines-owning-memory)
}

bool XR806Preferences::load_from_key(uint32_t key, uint8_t *dest, size_t len) {
  return dest != nullptr && len != 0 && this->open_() && this->journal_.get(key, dest, len);
}

bool XR806Preferences::save_to_key(uint32_t key, const uint8_t *src, size_t len) {
  if (src == nullptr || len == 0 || !this->open_())
    return false;
  if (this->journal_.put(key, src, len) == PreferenceJournal<SNAPSHOT_SIZE>::PutResult::PUT_RESULT_FULL) {
    ESP_LOGE(TAG, "No room for %u more bytes of preferences", static_cast<unsigned>(len));
    return false;
  }
  return true;
}

bool XR806Preferences::sync() {
  if (!this->open_())
    return false;
  if (!this->journal_.dirty())
    return true;
  if (!this->journal_.commit(fdcm_storage)) {
    ESP_LOGW(TAG, "Writing preferences failed; the previous ones stay in place");
    return false;
  }
  ESP_LOGV(TAG, "Saved preferences to sector %u (generation %" PRIu32 ")", this->journal_.active_slot(),
           this->journal_.generation());
  return true;
}

bool XR806Preferences::reset() { return this->open_() && this->journal_.clear(fdcm_storage); }

bool XR806PreferenceBackend::save(const uint8_t *src, size_t len) {
  return len == this->length_ && this->owner_->save_to_key(this->key_, src, len);
}

bool XR806PreferenceBackend::load(uint8_t *dest, size_t len) {
  return len == this->length_ && this->owner_->load_from_key(this->key_, dest, len);
}

static XR806Preferences xr806_preferences;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::xr806

namespace esphome {
ESPPreferences *global_preferences =
    &xr806::xr806_preferences;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
}  // namespace esphome

#endif  // USE_XR806
