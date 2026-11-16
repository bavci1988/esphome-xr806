#include "wlan_fw_swap.h"

#if defined(USE_XR806) && defined(USE_XR806_API_KEEPALIVE_OFFLOAD)

#include <cinttypes>
#include <cstring>

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

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
volatile bool serve_patched = false;
volatile uint32_t served_bytes = 0;
}  // namespace

extern "C" {
uint32_t __real_image_rw(uint32_t id, image_seg_t seg, uint32_t offset, void *buf, uint32_t size, int do_write);

// The driver reads the WLAN firmware with image_rw() (from wlan_bin_read(), which calls itself inside the vendor
// library and so cannot be wrapped). Body reads of that section come from the patched copy; it has the same size
// as the stock firmware, so the section header stays valid.
uint32_t __wrap_image_rw(uint32_t id, image_seg_t seg, uint32_t offset, void *buf, uint32_t size, int do_write) {
  if (!serve_patched || do_write || seg != IMAGE_SEG_BODY || id != WLAN_FW_SECTION_ID)
    return __real_image_rw(id, seg, offset, buf, size, do_write);
  const auto total = static_cast<uint32_t>(xr806_wlan_fw_keepalive_end - xr806_wlan_fw_keepalive);
  if (offset >= total)
    return 0;
  const uint32_t count = std::min(size, total - offset);
  memcpy(buf, xr806_wlan_fw_keepalive + offset, count);
  served_bytes = served_bytes + count;
  return count;
}
}

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.wlan_fw_swap");

static constexpr uint32_t SWAP_AFTER_MS = 60000;
static constexpr uint32_t VERIFY_MS = 120000;
static constexpr uint32_t RESTART_DELAY_MS = 500;

static bool patched_active = false;

bool wlan_fw_patched_active() { return patched_active; }

void XR806WlanFirmwareSwap::dump_config() {
  ESP_LOGCONFIG(
      TAG, "XR806 WLAN firmware swap: patched keepalive firmware (%u bytes) %" PRIu32 " s after the station connected",
      static_cast<unsigned>(xr806_wlan_fw_keepalive_end - xr806_wlan_fw_keepalive), SWAP_AFTER_MS / 1000);
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
      if (now - this->since_ms_ < RESTART_DELAY_MS)
        return;
      this->restart_wlan_(true);
      wifi->enable();
      this->phase_ = Phase::VERIFY;
      this->since_ms_ = now;
      return;
    case Phase::VERIFY:
      if (connected) {
        patched_active = true;
        ESP_LOGI(TAG, "Station connected with the patched WLAN firmware");
        this->phase_ = Phase::ACTIVE;
        return;
      }
      if (now - this->since_ms_ < VERIFY_MS)
        return;
      ESP_LOGW(TAG, "No connection with the patched WLAN firmware; back to the stock firmware until the next boot");
      wifi->disable();
      this->restart_wlan_(false);
      wifi->enable();
      this->phase_ = Phase::STOCK;
      return;
    case Phase::ACTIVE:
    case Phase::STOCK:
      this->disable_loop();
      return;
  }
}

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_XR806_API_KEEPALIVE_OFFLOAD
