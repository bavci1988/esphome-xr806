#include "wlan_fw_swap.h"

#if defined(USE_XR806) && defined(USE_XR806_API_KEEPALIVE_OFFLOAD)

#include <cinttypes>
#include <cstring>

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/core/log.h"
#include "wlan_mib.h"

extern "C" {
// The network framework declarations depend on the project configuration.
#include "xr_config.h"
#include "demo/trv16_port/prj_config.h"
#include "common/framework/net_ctrl.h"
#include "image/image.h"

// The patched firmware, linked from build_support/wlan_fw_keepalive_blob.S into XIP flash.
extern const uint8_t xr806_wlan_fw_keepalive[];
extern const uint8_t xr806_wlan_fw_keepalive_end[];
}

namespace {
// Image section of wlan_fw.bin (project/image_cfg/image.cfg).
constexpr uint32_t WLAN_FW_SECTION_ID = 0xA5F95A06;
// Initialised data, so it holds before the SDK loads the WLAN firmware at platform start, ahead of setup().
#ifdef USE_XR806_WLAN_FW_LOAD_AT_BOOT
volatile bool serve_patched = true;
#else
volatile bool serve_patched = false;
#endif
volatile uint32_t served_bytes = 0;
}  // namespace

extern "C" {
uint32_t __real_image_rw(uint32_t id, image_seg_t seg, uint32_t offset, void *buf, uint32_t size, int do_write);

// The driver reads the WLAN firmware with image_rw() (from wlan_bin_read(), which calls itself inside the vendor
// library and so cannot be wrapped) and expects exactly the body size of the section header, which can be larger
// than the file. So the flash is read as usual and the patched bytes, same length as the stock file, are laid over
// it; padding and return value stay those of the flash.
uint32_t __wrap_image_rw(uint32_t id, image_seg_t seg, uint32_t offset, void *buf, uint32_t size, int do_write) {
  const uint32_t result = __real_image_rw(id, seg, offset, buf, size, do_write);
  if (do_write || seg != IMAGE_SEG_BODY || id != WLAN_FW_SECTION_ID)
    return result;
  if (offset == 0) {
    // The WLAN core's 32 kHz clock must come from the calibration unit: on the raw RC clock the core does not wake
    // by its own timer in standby. The power control registers keep their values across a restart (earlier
    // experiments left them changed), and the driver reports the clock to the core right after this firmware read.
    volatile uint32_t &clock_32k = *reinterpret_cast<volatile uint32_t *>(0x4004023C);
    volatile uint32_t &rc_measurement = *reinterpret_cast<volatile uint32_t *>(0x40040010);
    volatile uint32_t &power_config = *reinterpret_cast<volatile uint32_t *>(0x40040074);
    clock_32k = clock_32k | (1U << 3);
    rc_measurement = rc_measurement & ~1U;
    // Bits 6-7: the calibration starts when the CPU and the WLAN core both sleep (SDK setting).
    power_config = (power_config & ~(3U << 6)) | (2U << 6);
  }
  if (!serve_patched)
    return result;
  const auto total = static_cast<uint32_t>(xr806_wlan_fw_keepalive_end - xr806_wlan_fw_keepalive);
  if (offset < total) {
    const uint32_t count = std::min(result, total - offset);
    memcpy(buf, xr806_wlan_fw_keepalive + offset, count);
    served_bytes = served_bytes + count;
  }
  return result;
}
}

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.wlan_fw_swap");

static constexpr uint32_t SWAP_AFTER_MS = 60000;
static constexpr uint32_t VERIFY_MS = 120000;
static constexpr uint32_t RESTART_DELAY_MS = 500;
// With load_at_boot: after this many starts in a row without a confirmed connection on the patched firmware, the
// stock one is used until the next firmware update.
static constexpr uint8_t BOOT_ATTEMPTS = 3;
// After a hang the driver's own recovery task runs for a moment; requests to the driver in that window fault the CPU.
static constexpr uint32_t RECOVERY_DELAY_MS = 3000;
// A second hang this soon after a restart of the core: the device restarts instead.
static constexpr uint32_t RECOVERY_REPEAT_MS = 10 * 60 * 1000;

// Read back from the running WLAN core to confirm that the firmware with the modules was loaded: the receive hook
// of the function table no longer points at the vendor keepalive, and the sleep module's record starts with its
// magic word, which module_init writes (build_support/wlan_keepalive/sleep.c). The record alone could be left from
// an earlier start of the core. Places in the firmware's initialisation code (below 0x09001884) cannot be used:
// the core reuses that memory after its start.
static constexpr uint32_t RECEIVE_HOOK_ADDRESS = 0xFFF0072C;
static constexpr uint32_t RECEIVE_HOOK_STOCK = 0x0900A013;
static constexpr uint32_t MODULE_RECORD_ADDRESS = 0x0900BEA8;
static constexpr uint32_t MODULE_RECORD_MAGIC = 0x31504C53;

static bool patched_active = false;
static XR806WlanFirmwareSwap *instance = nullptr;

static bool patched_code_running() {
  uint32_t hook = 0;
  uint32_t magic = 0;
  const int hook_err = wlan_read_words(RECEIVE_HOOK_ADDRESS, &hook, 1);
  const int magic_err = wlan_read_words(MODULE_RECORD_ADDRESS, &magic, 1);
  if (hook_err == 0 && magic_err == 0 && (hook >> 24) == 0x09 && hook != RECEIVE_HOOK_STOCK &&
      magic == MODULE_RECORD_MAGIC)
    return true;
  ESP_LOGW(TAG, "Firmware check: receive hook %08" PRIX32 " (read %d), module record %08" PRIX32 " (read %d)", hook,
           hook_err, magic, magic_err);
  return false;
}

bool wlan_fw_patched_active() { return patched_active; }

bool wlan_fw_recover_after_hang() { return instance != nullptr && instance->request_recovery(); }

bool XR806WlanFirmwareSwap::request_recovery() {
  auto *wifi = wifi::global_wifi_component;
  const uint32_t now = millis();
  if (wifi == nullptr || this->recovering_ ||
      (this->recovered_ms_ != 0 && now - this->recovered_ms_ < RECOVERY_REPEAT_MS))
    return false;
  this->restart_patched_ = patched_active || this->phase_ == Phase::VERIFY || this->phase_ == Phase::RESTART;
  ESP_LOGW(TAG, "Restarting the WLAN core with the %s firmware after it hung in standby",
           this->restart_patched_ ? LOG_STR_LITERAL("patched") : LOG_STR_LITERAL("stock"));
  patched_active = false;
  this->recovering_ = true;
  this->recovered_ms_ = now;
  wifi->disable();
  this->phase_ = Phase::RESTART;
  this->since_ms_ = now;
  this->enable_loop();
  return true;
}

void XR806WlanFirmwareSwap::setup() {
  instance = this;
#ifdef USE_XR806_WLAN_FW_LOAD_AT_BOOT
  // Counts starts on the patched firmware; a confirmed connection resets it. Keyed by the build, so an update
  // tries the patched firmware again.
  this->attempts_pref_ =
      global_preferences->make_preference<uint8_t>(fnv1_hash("xr806_wlan_fw") ^ App.get_config_hash());
  uint8_t attempts = 0;
  this->attempts_pref_.load(&attempts);
  if (attempts >= BOOT_ATTEMPTS) {
    ESP_LOGW(TAG, "Patched WLAN firmware failed %u starts in a row; stock firmware until the next update", attempts);
    this->phase_ = Phase::FALLBACK;
    return;
  }
  ESP_LOGI(TAG, "Patched WLAN firmware served at boot (%" PRIu32 " bytes), start %u of %u", served_bytes, attempts + 1,
           BOOT_ATTEMPTS);
  ++attempts;
  this->attempts_pref_.save(&attempts);
  global_preferences->sync();
  this->phase_ = Phase::VERIFY;
#endif
}

void XR806WlanFirmwareSwap::fall_back_(wifi::WiFiComponent *wifi) {
  wifi->disable();
  this->restart_wlan_(false);
  wifi->enable();
  this->phase_ = Phase::STOCK;
}

void XR806WlanFirmwareSwap::dump_config() {
#ifdef USE_XR806_WLAN_FW_LOAD_AT_BOOT
  ESP_LOGCONFIG(TAG, "XR806 WLAN firmware swap: patched keepalive firmware (%u bytes) loaded at boot",
                static_cast<unsigned>(xr806_wlan_fw_keepalive_end - xr806_wlan_fw_keepalive));
#else
  ESP_LOGCONFIG(
      TAG, "XR806 WLAN firmware swap: patched keepalive firmware (%u bytes) %" PRIu32 " s after the station connected",
      static_cast<unsigned>(xr806_wlan_fw_keepalive_end - xr806_wlan_fw_keepalive), SWAP_AFTER_MS / 1000);
#endif
}

void XR806WlanFirmwareSwap::restart_wlan_(bool patched) {
  served_bytes = 0;
  serve_patched = patched;
  net_sys_onoff(0);
  net_sys_onoff(1);
  ESP_LOGI(TAG, "WLAN core restarted with the %s firmware (%" PRIu32 " patched bytes served)",
           patched ? LOG_STR_LITERAL("patched") : LOG_STR_LITERAL("stock"), served_bytes);
}

void XR806WlanFirmwareSwap::loop() {
  auto *wifi = wifi::global_wifi_component;
  const uint32_t now = App.get_loop_component_start_time();
  const bool connected = wifi != nullptr && wifi->is_connected();
  switch (this->phase_) {
    case Phase::WAIT:
      if (!connected) {
        this->since_ms_ = 0;
        return;
      }
      if (this->since_ms_ == 0) {
        this->since_ms_ = now;
        ESP_LOGI(TAG, "Patched WLAN firmware in %" PRIu32 " s", SWAP_AFTER_MS / 1000);
        return;
      }
      if (now - this->since_ms_ < SWAP_AFTER_MS)
        return;
      ESP_LOGI(TAG, "Loading the patched WLAN firmware");
      wifi->disable();
      this->phase_ = Phase::RESTART;
      this->since_ms_ = now;
      return;
    case Phase::RESTART:
      if (now - this->since_ms_ < (this->recovering_ ? RECOVERY_DELAY_MS : RESTART_DELAY_MS))
        return;
      this->restart_wlan_(this->restart_patched_);
      wifi->enable();
      this->phase_ = Phase::VERIFY;
      this->since_ms_ = now;
      return;
    case Phase::VERIFY:
      if (this->since_ms_ == 0)
        this->since_ms_ = now;
      if (connected) {
        if (this->recovering_) {
          this->recovering_ = false;
          ESP_LOGI(TAG, "Station connected again after the WLAN core restart");
          if (!this->restart_patched_) {
            this->phase_ = Phase::STOCK;
            return;
          }
        }
        if (patched_code_running()) {
          patched_active = true;
          ESP_LOGI(TAG, "Station connected; the WLAN core runs the patched firmware");
#ifdef USE_XR806_WLAN_FW_LOAD_AT_BOOT
          uint8_t attempts = 0;
          this->attempts_pref_.save(&attempts);
          global_preferences->sync();
#endif
          this->phase_ = Phase::ACTIVE;
          return;
        }
        ESP_LOGW(TAG, "The WLAN core does not run the patched code; stock firmware until the next boot");
        this->fall_back_(wifi);
        return;
      }
      if (now - this->since_ms_ < VERIFY_MS)
        return;
      if (this->recovering_) {
        ESP_LOGE(TAG, "No connection after the WLAN core restart; restarting");
        global_preferences->sync();
        delay(100);  // let the log reach the UART
        arch_restart();
      }
      ESP_LOGW(TAG, "No connection with the patched WLAN firmware; back to the stock firmware until the next boot");
      this->fall_back_(wifi);
      return;
    case Phase::FALLBACK:
      if (wifi == nullptr)
        return;
      this->fall_back_(wifi);
      return;
    case Phase::ACTIVE:
    case Phase::STOCK:
      this->disable_loop();
      return;
  }
}

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_XR806_API_KEEPALIVE_OFFLOAD
