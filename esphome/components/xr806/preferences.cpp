#include "esphome/core/defines.h"

#ifdef USE_XR806

#include "preferences.h"

#include <cstring>

#include "esphome/core/log.h"

extern "C" {
#include "demo/trv16_port/prj_config.h"
#include "image/flash.h"
}

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.preferences");

// Keep these configuration-time checks adjacent to the only firmware path
// which can erase the reserved WXU tail sectors.
static_assert(PRJCONF_ESPHOME_PREFS_FLASH == PRJCONF_SYSINFO_FLASH);
static_assert(PRJCONF_ESPHOME_PREFS_SECTOR == 4096);
static_assert(PRJCONF_ESPHOME_PREFS0_ADDR + PRJCONF_ESPHOME_PREFS_SECTOR == PRJCONF_ESPHOME_PREFS1_ADDR);
static_assert(PRJCONF_ESPHOME_PREFS1_ADDR + PRJCONF_ESPHOME_PREFS_SECTOR == PRJCONF_SYSINFO_ADDR);
static_assert(PRJCONF_SYSINFO_ADDR == 1980 * 1024);
static_assert(PRJCONF_SYSINFO_ADDR + PRJCONF_SYSINFO_SIZE == 1984 * 1024);
static_assert(PRJCONF_ESPHOME_PREFS0_ADDR >= 1920 * 1024);
static_assert(PRJCONF_ESPHOME_PREFS0_ADDR == 1972 * 1024);
static_assert(PRJCONF_ESPHOME_PREFS1_ADDR == 1976 * 1024);

// Snapshot: 16-byte header, followed by packed entries {key32, len16, data[len]}.
// The CRC includes the generation and length, preventing a damaged generation
// counter from making a corrupt sector appear newer than a valid one.
// Two FDCM sectors are alternated; the previously committed one is never
// modified until after a complete, validated write to the other sector.
uint32_t XR806Preferences::checksum_(const uint8_t *data, size_t used) const {
  return preference_snapshot_checksum(data, used);
}

bool XR806Preferences::valid_snapshot_(const uint8_t *data, SnapshotHeader *header) const {
  return preference_snapshot_valid(data, SNAPSHOT_SIZE, MAGIC, VERSION, header);
}

bool XR806Preferences::read_slot_(uint8_t slot, SnapshotHeader *header) {
  if (fdcm_read(this->slots_[slot], this->scratch_, SNAPSHOT_SIZE) != SNAPSHOT_SIZE)
    return false;
  return this->valid_snapshot_(this->scratch_, header);
}

// A foreign flash sector must NEVER be erased by FDCM's automatic rewrite.
// The original Tuya 2 MiB dump includes nonblank OEM data after 2004 KiB;
// permit only completely erased sectors or matching FDCM snapshot headers.
// Read the entire 4 KiB sector so a blank prefix cannot hide other data.
static bool safe_preference_sector(uint32_t address) {
  uint8_t probe[64];
  uint32_t magic = 0;
  uint16_t payload_length = 0;
  bool all_erased = true;
  for (uint32_t offset = 0; offset < PRJCONF_ESPHOME_PREFS_SECTOR; offset += sizeof(probe)) {
    if (flash_read(PRJCONF_ESPHOME_PREFS_FLASH, address + offset, probe, sizeof(probe)) != sizeof(probe))
      return false;
    if (offset == 0) {
      memcpy(&magic, probe, sizeof(magic));
      memcpy(&payload_length, probe + 6, sizeof(payload_length));
    }
    for (uint8_t value : probe) {
      if (value != 0xFF) {
        all_erased = false;
        break;
      }
    }
  }
  constexpr uint32_t FDCM_ID_CODE = 0xA55A5AA5;
  return all_erased || (magic == FDCM_ID_CODE && payload_length == 2048);
}

bool XR806Preferences::open_() {
  if (this->initialized_)
    return this->available_;
  this->initialized_ = true;

  // Fail closed on any unexpected sector contents before fdcm_write() can
  // erase foreign Tuya calibration/credentials/metadata.
  if (!safe_preference_sector(PRJCONF_ESPHOME_PREFS0_ADDR) ||
      !safe_preference_sector(PRJCONF_ESPHOME_PREFS1_ADDR)) {
    ESP_LOGE(TAG, "Nonblank foreign flash data in reserved preference sectors; refusing writes");
    return false;
  }

  // The SDK's platform_init() initializes flash/FDCM before ESPHome setup().
  this->slots_[0] = fdcm_open(PRJCONF_ESPHOME_PREFS_FLASH, PRJCONF_ESPHOME_PREFS0_ADDR,
                             PRJCONF_ESPHOME_PREFS_SECTOR);
  this->slots_[1] = fdcm_open(PRJCONF_ESPHOME_PREFS_FLASH, PRJCONF_ESPHOME_PREFS1_ADDR,
                             PRJCONF_ESPHOME_PREFS_SECTOR);
  if (this->slots_[0] == nullptr || this->slots_[1] == nullptr) {
    ESP_LOGE(TAG, "Both FDCM preference sectors must be accessible; refusing writes");
    return false;
  }

  SnapshotHeader h0{};
  SnapshotHeader h1{};
  const bool valid0 = this->read_slot_(0, &h0);
  if (valid0) {
    memcpy(this->data_, this->scratch_, SNAPSHOT_SIZE);
    this->active_slot_ = 0;
    this->generation_ = h0.generation;
    this->used_ = h0.used;
  }
  const bool valid1 = this->read_slot_(1, &h1);
  if (valid1 && (!valid0 || preference_generation_newer(h1.generation, this->generation_))) {
    memcpy(this->data_, this->scratch_, SNAPSHOT_SIZE);
    this->active_slot_ = 1;
    this->generation_ = h1.generation;
    this->used_ = h1.used;
  }

  this->available_ = true;
  if (valid0 || valid1) {
    ESP_LOGD(TAG, "Loaded preference snapshot from sector %u (generation %lu)", this->active_slot_,
             static_cast<unsigned long>(this->generation_));
  } else {
    ESP_LOGD(TAG, "No valid saved preferences; starting empty");
  }
  return true;
}

bool XR806Preferences::load_from_key(uint32_t key, uint8_t *dest, size_t len) {
  if (dest == nullptr || len == 0 || !this->open_())
    return false;

  const size_t end = sizeof(SnapshotHeader) + this->used_;
  for (size_t cursor = sizeof(SnapshotHeader); cursor < end;) {
    uint32_t stored_key;
    uint16_t stored_len;
    memcpy(&stored_key, this->data_ + cursor, sizeof(stored_key));
    memcpy(&stored_len, this->data_ + cursor + 4, sizeof(stored_len));
    if (stored_key == key && stored_len == len) {
      memcpy(dest, this->data_ + cursor + 6, len);
      return true;
    }
    cursor += 6 + stored_len;
  }
  return false;
}

bool XR806Preferences::save_to_key(uint32_t key, const uint8_t *src, size_t len) {
  if (src == nullptr || len == 0 || len > UINT16_MAX || !this->open_())
    return false;

  // In-place writes would not preserve the old snapshot on capacity failures.
  // Build the new record set separately and only publish it if it fits.
  memset(this->scratch_, 0, SNAPSHOT_SIZE);
  size_t cursor_out = sizeof(SnapshotHeader);
  const size_t end = sizeof(SnapshotHeader) + this->used_;
  for (size_t cursor = sizeof(SnapshotHeader); cursor < end;) {
    uint32_t stored_key;
    uint16_t stored_len;
    memcpy(&stored_key, this->data_ + cursor, sizeof(stored_key));
    memcpy(&stored_len, this->data_ + cursor + 4, sizeof(stored_len));
    const size_t bytes = 6 + stored_len;
    if (stored_key != key) {
      memcpy(this->scratch_ + cursor_out, this->data_ + cursor, bytes);
      cursor_out += bytes;
    }
    cursor += bytes;
  }

  if (len + 6 > SNAPSHOT_SIZE - cursor_out) {
    ESP_LOGE(TAG, "Preference record exceeds 2 KiB snapshot capacity");
    return false;
  }
  const uint16_t record_len = static_cast<uint16_t>(len);
  memcpy(this->scratch_ + cursor_out, &key, sizeof(key));
  memcpy(this->scratch_ + cursor_out + 4, &record_len, sizeof(record_len));
  memcpy(this->scratch_ + cursor_out + 6, src, len);
  cursor_out += 6 + len;

  this->used_ = cursor_out - sizeof(SnapshotHeader);
  memcpy(this->data_, this->scratch_, SNAPSHOT_SIZE);
  this->dirty_ = true;
  return true;
}

bool XR806Preferences::sync() {
  if (!this->open_())
    return false;
  if (!this->dirty_)
    return true;

  const uint8_t target = this->active_slot_ ^ 1U;
  const SnapshotHeader header{MAGIC, VERSION, static_cast<uint16_t>(this->used_), this->generation_ + 1, 0};
  memcpy(this->data_, &header, sizeof(header));
  const uint32_t checksum = this->checksum_(this->data_, this->used_);
  memcpy(this->data_ + 12, &checksum, sizeof(checksum));

  if (fdcm_write(this->slots_[target], this->data_, SNAPSHOT_SIZE) != SNAPSHOT_SIZE) {
    ESP_LOGE(TAG, "Cannot write preference snapshot to FDCM sector %u", target);
    return false;
  }

  SnapshotHeader verify{};
  if (!this->read_slot_(target, &verify) || memcmp(this->scratch_, this->data_, SNAPSHOT_SIZE) != 0) {
    ESP_LOGE(TAG, "Preference write verification failed; preserving previous sector");
    return false;
  }

  this->active_slot_ = target;
  this->generation_ = header.generation;
  this->dirty_ = false;
  return true;
}

bool XR806Preferences::reset() {
  if (!this->open_())
    return false;
  // Commit a newer, empty snapshot first. An interrupted reset therefore
  // boots either the old valid state or the fully committed empty state.
  const size_t old_used = this->used_;
  this->used_ = 0;
  this->dirty_ = true;
  if (!this->sync()) {
    this->used_ = old_used;
    return false;
  }
  // The inactive old sector may contain credentials; erase it after the
  // newer empty snapshot has been verified.
  return fdcm_erase(this->slots_[this->active_slot_ ^ 1U]) == 0;
}

ESPPreferenceObject XR806Preferences::make_preference(size_t length, uint32_t type, bool /*in_flash*/) {
  if (length == 0 || length > UINT16_MAX)
    return {};
  // ESPPreferenceObject owns only a pointer, matching other platform
  // preference backends whose handles persist until device reboot.
  return ESPPreferenceObject(new XR806PreferenceBackend(this, type, length));  // NOLINT(cppcoreguidelines-owning-memory)
}

bool XR806PreferenceBackend::save(const uint8_t *src, size_t len) {
  return len == this->length_ && this->owner_->save_to_key(this->key_, src, len);
}

bool XR806PreferenceBackend::load(uint8_t *dest, size_t len) {
  return len == this->length_ && this->owner_->load_from_key(this->key_, dest, len);
}

static XR806Preferences xr806_preferences;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::xr806

namespace esphome {
ESPPreferences *global_preferences =  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    &xr806::xr806_preferences;
}  // namespace esphome

#endif  // USE_XR806
