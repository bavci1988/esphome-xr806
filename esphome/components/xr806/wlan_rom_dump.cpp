#include "wlan_rom_dump.h"

#if defined(USE_XR806) && defined(USE_XR806_WLAN_ROM_DUMP)

#include <cinttypes>
#include <cstring>

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "wlan_mib.h"

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.wlan_rom_dump");

static constexpr uint32_t WORDS_PER_READ = 16;
static constexpr uint32_t START_DELAY_MS = 60000;  // time for an OTA update before the dump
// Patch RAM of the WLAN core and its first words in bin/xradio_v3/wlan_fw.bin.
static constexpr uint32_t CHECK_ADDRESS = 0xFFF00000;
// Word 0 (a NOP in the file) is rewritten by the firmware at run time, so the check starts at word 1.
static constexpr uint32_t CHECK_WORDS[] = {0xE1A00000, 0xE59F20A4, 0xEE012F10, 0xE59F00A0};
static constexpr size_t CHECK_FIRST = 1;
// ROM of the WLAN core: the patch's function table points up to 0x1F46B.
static constexpr uint32_t ROM_END = 0x20000;

void XR806WlanRomDump::dump_config() {
  ESP_LOGCONFIG(TAG, "XR806 WLAN ROM dump: starts %" PRIu32 " s after the station connected", START_DELAY_MS / 1000);
}

bool XR806WlanRomDump::read_words_(uint32_t address, uint32_t *words, uint32_t count) {
  const int err = wlan_read_words(address, words, count);
  if (err != 0) {
    ESP_LOGE(TAG, "MIB 9 read at 0x%08" PRIX32 " failed (%d)", address, err);
    return false;
  }
  return true;
}

void XR806WlanRomDump::check_() {
  uint32_t words[WORDS_PER_READ];
  if (!this->read_words_(CHECK_ADDRESS, words, WORDS_PER_READ)) {
    this->phase_ = Phase::DONE;
    return;
  }
  bool match = true;
  for (size_t i = CHECK_FIRST; i < sizeof(CHECK_WORDS) / sizeof(CHECK_WORDS[0]); ++i)
    match = match && words[i] == CHECK_WORDS[i];
  ESP_LOGI(TAG, "Check 0x%08" PRIX32 ": %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " -> %s", CHECK_ADDRESS,
           words[0], words[1], words[2], words[3], match ? "memory is readable" : "NO MATCH");
  if (!match) {
    ESP_LOGW(TAG, "The request does not return memory; no ROM dump");
    this->phase_ = Phase::DONE;
    return;
  }
  ESP_LOGI(TAG, "ROM dump 0x00000000..0x%08" PRIX32 " begins", ROM_END - 1);
  this->phase_ = Phase::DUMP;
}

void XR806WlanRomDump::dump_step_() {
  uint32_t words[WORDS_PER_READ];
  if (!this->read_words_(this->next_address_, words, WORDS_PER_READ)) {
    ESP_LOGE(TAG, "ROM dump stopped at 0x%08" PRIX32, this->next_address_);
    this->phase_ = Phase::DONE;
    return;
  }
  // Little-endian bytes as they lie in memory, so the lines join into a binary image.
  uint8_t bytes[4 * WORDS_PER_READ];
  memcpy(bytes, words, sizeof(bytes));
  char hex[2 * sizeof(bytes) + 1];
  format_hex_to(hex, bytes, sizeof(bytes));
  ESP_LOGI(TAG, "WROM %08" PRIX32 " %s", this->next_address_, hex);
  for (uint8_t b : bytes) {
    this->crc_ ^= b;
    for (int k = 0; k < 8; ++k)
      this->crc_ = (this->crc_ >> 1) ^ (0xEDB88320 & (0u - (this->crc_ & 1)));
  }
  this->next_address_ += sizeof(bytes);
  if (this->next_address_ >= ROM_END) {
    ESP_LOGI(TAG, "ROM dump done: %" PRIu32 " bytes, CRC-32 %08" PRIX32, ROM_END, ~this->crc_);
    this->phase_ = Phase::DONE;
  }
}

void XR806WlanRomDump::loop() {
  switch (this->phase_) {
    case Phase::WAIT: {
      if (wifi::global_wifi_component == nullptr || !wifi::global_wifi_component->is_connected()) {
        this->connected_ms_ = 0;
        return;
      }
      const uint32_t now = App.get_loop_component_start_time();
      if (this->connected_ms_ == 0) {
        this->connected_ms_ = now;
        ESP_LOGI(TAG, "WLAN ROM dump in %" PRIu32 " s", START_DELAY_MS / 1000);
        return;
      }
      if (now - this->connected_ms_ >= START_DELAY_MS)
        this->phase_ = Phase::CHECK;
      return;
    }
    case Phase::CHECK:
      this->check_();
      return;
    case Phase::DUMP:
      this->dump_step_();
      return;
    case Phase::DONE:
      this->disable_loop();
      return;
  }
}

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_XR806_WLAN_ROM_DUMP
