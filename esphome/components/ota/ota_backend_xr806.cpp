#include "esphome/core/defines.h"
#ifdef USE_XR806
#include "ota_backend_xr806.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cstring>
#include <limits>
extern "C" {
#include "xr_config.h"
#include "image/image.h"
#include "ota/ota.h"
#include "pm/pm.h"
#include "driver/chip/hal_flash.h"
#include "image/flash.h"
}
#include "xz/xz.h"
#if !defined(CONFIG_OTA) || !defined(CONFIG_OTA_POLICY_IMAGE_COMPRESSION)
#error "XR806 native OTA requires SDK compression OTA policy and AWIH/XZ images"
#endif
#if !defined(CONFIG_PM) || !defined(CONFIG_PM_WAKELOCKS)
#error "XR806 native OTA requires real PM wake locks"
#endif
#ifdef USE_XR806_STANDBY_CONTROLLER
#include "esphome/components/xr806/lowpower_policy.h"
#include "esphome/components/xr806/standby_controller.h"
#endif
namespace esphome::ota {
ESPHOME_LOG_TAG(TAG, "ota.xr806");
static struct wakelock ota_native_wakelock = {};
std::unique_ptr<XR806OTABackend> make_ota_backend() { return std::make_unique<XR806OTABackend>(); }
XR806OTABackend::~XR806OTABackend() { this->abort(); }

OTAResponseTypes XR806OTABackend::begin(size_t image_size, OTAType type) {
  if (type != OTA_TYPE_UPDATE_APP)
    return OTA_RESPONSE_ERROR_UNSUPPORTED_OTA_TYPE;
  if (!this->update_lock_.try_acquire())
    return OTA_RESPONSE_ERROR_UPDATE_BUSY;
  // A previous aborted transfer must not poison the next OTA attempt.
  this->received_ = 0;
  this->declared_ = 0;
  this->required_ = 0;
  this->preface_len_ = 0;
  this->md5_supplied_ = false;
  this->md5_valid_ = false;
  this->writing_ = false;
  this->committed_ = false;
  if (image_size > std::numeric_limits<uint32_t>::max()) {
    this->update_lock_.release();
    return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
  }

  const image_ota_param_t *runtime = image_get_ota_param();
  if (runtime == nullptr) {
    ESP_LOGE(TAG, "Cannot read XR806 ROM OTA geometry");
    this->update_lock_.release();
    return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
  }
  // A wired AWIH flash leaves FDCM empty. ROM and bootloader then boot seq0, and the
  // first ota_push_finish() writes the FDCM header itself, so it is not initialized here.
  image_cfg_t cfg{};
  const bool has_cfg = image_get_cfg(&cfg) == 0;
  const image_seq_t running = image_get_running_seq();
  if (running >= IMAGE_SEQ_NUM || running != runtime->running_seq) {
    this->update_lock_.release();
    return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
  }

  const uint8_t flash_id = runtime->flash[running];
  struct FlashDev *dev = getFlashDev(flash_id);
  struct FlashChip *chip = dev != nullptr ? getFlashChip(dev) : nullptr;
  if (chip == nullptr || chip->cfg.mSize == 0) {
    this->update_lock_.release();
    return OTA_RESPONSE_ERROR_WRONG_CURRENT_FLASH_CONFIG;
  }

  xr806::WriterGeometry g{};
  g.flash_size = chip->cfg.mSize;  // JEDEC-probed physical flash capacity
  g.metadata_addr = runtime->ota_addr;
  g.metadata_size = runtime->ota_size;
  g.metadata_flash_id = static_cast<uint8_t>(runtime->ota_flash);
  g.image_addr[0] = runtime->addr[0];
  g.image_addr[1] = runtime->addr[1];
  g.flash_id[0] = runtime->flash[0];
  g.flash_id[1] = runtime->flash[1];
  g.image_max_kib = runtime->img_max_size;
  g.compressed_max_kib = runtime->img_xz_max_size;
  g.bootloader_bytes = runtime->bl_size;
  g.upload_bytes = static_cast<uint32_t>(image_size);
  g.running_seq = running;
  // image_get_cfg() fails for an empty FDCM, which boots seq0. A selection that reads
  // back as anything other than verified seq0 remains blocked.
  g.selected_seq = has_cfg ? cfg.seq : 0;
  g.selected_verified = has_cfg ? cfg.state == IMAGE_STATE_VERIFIED : true;
  this->plan_ = xr806::validate_writer(g);
  if (this->plan_.error != xr806::WriterError::OK) {
    ESP_LOGE(TAG, "XR806 OTA runtime layout rejected (%u): active=0x%X staging=0x%X FDCM=0x%X",
             static_cast<unsigned>(this->plan_.error), g.image_addr[0], g.image_addr[1], g.metadata_addr);
    this->update_lock_.release();
    return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
  }
#ifdef USE_XR806_STANDBY_CONTROLLER
  xr806::lowpower_policy().set_ota_idle(false);
  if (auto *controller = xr806::standby_controller(); controller != nullptr)
    controller->request_standby(false);
#endif
  ota_native_wakelock.name = const_cast<char *>("xr806.ota.flash");
  if (pm_wake_lock(&ota_native_wakelock) != 0) {
    this->update_lock_.release();
    return OTA_RESPONSE_ERROR_UPDATE_BUSY;
  }
  this->pm_held_ = true;
  if (ota_push_init() != OTA_STATUS_OK || ota_set_skip_size(0) != OTA_STATUS_OK) {
    this->stop_();
    return OTA_RESPONSE_ERROR_UPDATE_PREPARE;
  }
  this->sdk_initialized_ = true;
  this->image_base_ = runtime->bl_size;
  this->staging_flash_ = runtime->flash[this->plan_.candidate_seq];
  this->declared_ = static_cast<uint32_t>(image_size);
  this->digest_.init();
  ESP_LOGI(TAG, "Writable OTA: seq1=0x%X (%u bytes), active seq0=0x%X, FDCM=0x%X", this->plan_.candidate.start,
           this->plan_.candidate.size, this->plan_.active.start, this->plan_.metadata.start);
  return OTA_RESPONSE_OK;
}

void XR806OTABackend::set_update_md5(const char *md5) {
  this->md5_supplied_ = true;
  this->md5_valid_ = md5 != nullptr && parse_hex(md5, this->expected_md5_, 16);
}

bool XR806OTABackend::push_(uint8_t *data, size_t bytes) {
  constexpr size_t CHUNK = 2048;
  while (bytes) {
    const uint32_t count = static_cast<uint32_t>(std::min(bytes, CHUNK));
    if (ota_push_data(data, count) != OTA_STATUS_OK)
      return false;
    data += count;
    bytes -= count;
  }
  return true;
}

OTAResponseTypes XR806OTABackend::write(uint8_t *data, size_t bytes) {
  if (!this->sdk_initialized_ || this->committed_ || (data == nullptr && bytes))
    return OTA_RESPONSE_ERROR_WRITING_FLASH;
  if (!bytes)
    return OTA_RESPONSE_OK;
  if (bytes > this->plan_.candidate.size - this->received_ ||
      (this->declared_ && bytes > this->declared_ - this->received_)) {
    this->abort();
    return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
  }
  this->digest_.add(data, bytes);
  this->received_ += static_cast<uint32_t>(bytes);
  if (!this->writing_) {
    const size_t take = std::min(bytes, sizeof(this->preface_) - this->preface_len_);
    std::memcpy(this->preface_ + this->preface_len_, data, take);
    this->preface_len_ += take;
    data += take;
    bytes -= take;
    if (this->preface_len_ < sizeof(this->preface_))
      return OTA_RESPONSE_OK;

    const auto image =
        xr806::validate_xz_preface(this->preface_, sizeof(this->preface_), this->plan_.candidate.size, this->declared_);
    if (!image.valid) {
      ESP_LOGE(TAG, "Wrong OTA image: require single AWIH/XZ APP wrapper, no erase performed");
      this->abort();
      return OTA_RESPONSE_ERROR_MAGIC;
    }
    this->required_ = image.bytes;
    // This is the REAL vendor flash ERASE, only after layout and format checks.
    if (ota_push_start() != OTA_STATUS_OK) {
      ESP_LOGE(TAG, "Vendor staging-sector erase failed");
      this->abort();
      return OTA_RESPONSE_ERROR_WRITING_FLASH;
    }
    this->writing_ = true;
    if (!this->push_(this->preface_, sizeof(this->preface_))) {
      this->abort();
      return OTA_RESPONSE_ERROR_WRITING_FLASH;
    }
  }
  if (bytes && !this->push_(data, bytes)) {
    ESP_LOGE(TAG, "Vendor staging flash write failed");
    this->abort();
    return OTA_RESPONSE_ERROR_WRITING_FLASH;
  }
  return OTA_RESPONSE_OK;
}

OTAResponseTypes XR806OTABackend::end() {
  if (!this->sdk_initialized_ || !this->writing_ || this->committed_ || !this->required_ ||
      this->received_ != this->required_ || (this->declared_ && this->received_ != this->declared_)) {
    this->abort();
    return OTA_RESPONSE_ERROR_UPDATE_END;
  }
  if (this->md5_supplied_) {
    this->digest_.calculate();
    if (!this->md5_valid_ || !this->digest_.equals_bytes(this->expected_md5_)) {
      this->abort();
      return OTA_RESPONSE_ERROR_MD5_MISMATCH;
    }
  }
  // The bootloader erases the active image before it decompresses the staged one and does not
  // stop on a decode error, so prove here that the stream decodes to a valid image.
  if (!this->probe_staged_image_()) {
    this->abort();
    return OTA_RESPONSE_ERROR_UPDATE_END;
  }
  // ota_push_finish() checks the wrapper sections and, through ota_verify_image(), writes
  // FDCM seq1=VERIFIED. That write is the commit: the next boot installs the update.
  const bool finished = ota_push_finish() == OTA_STATUS_OK;
  const bool selected = this->read_commit_();
  if (!finished && !selected) {
    this->abort();
    return OTA_RESPONSE_ERROR_UPDATE_END;
  }
  // From here FDCM may select seq1, so no further upload may touch the staging slot.
  this->committed_ = true;
  ota_push_stop();
  this->sdk_initialized_ = false;
  this->update_lock_.hold_until_reboot();
  // The wakelock is kept until the restart as well.
  if (!finished || !selected) {
    ESP_LOGE(TAG, "OTA commit incomplete (finish=%d, FDCM readback=%d); next boot state uncertain",
             static_cast<int>(finished), static_cast<int>(selected));
    return OTA_RESPONSE_ERROR_UPDATE_END;
  }
  ESP_LOGI(TAG, "XRadio OTA firmware committed; reboot required to install seq1 into seq0");
  return OTA_RESPONSE_OK;
}

bool XR806OTABackend::read_commit_() {
  image_cfg_t confirmed{};
  return image_get_cfg(&confirmed) == 0 && confirmed.seq == this->plan_.candidate_seq &&
         confirmed.state == IMAGE_STATE_VERIFIED;
}

bool XR806OTABackend::probe_staged_image_() {
  // Same decoder (ROM xz-embedded) and dictionary limit as the bootloader.
  constexpr size_t IN_CHUNK = 2048;
  constexpr size_t OUT_CHUNK = 4096;
  xz_crc32_init();
  struct xz_dec *dec = xz_dec_init(XZ_PREALLOC, xr806::XR806_BOOTLOADER_XZ_DICT);
  RAMAllocator<uint8_t> allocator;
  uint8_t *in = allocator.allocate(IN_CHUNK + OUT_CHUNK);
  if (dec == nullptr || in == nullptr) {
    if (dec != nullptr)
      xz_dec_end(dec);
    if (in != nullptr)
      allocator.deallocate(in, IN_CHUNK + OUT_CHUNK);
    ESP_LOGE(TAG, "Not enough memory to check the staged image");
    return false;
  }
  uint8_t *out = in + IN_CHUNK;
  xr806::DecompressedImageCheck check(this->image_base_, this->plan_.active.size);
  const uint32_t start = this->plan_.candidate.start + xr806::AWIH_HEADER_BYTES;
  const uint32_t length = this->required_ - xr806::AWIH_HEADER_BYTES;
  uint32_t offset = 0;
  xz_buf b{};
  b.out = out;
  b.out_size = OUT_CHUNK;
  enum xz_ret ret = XZ_OK;
  bool valid = true;
  while (valid && ret == XZ_OK) {
    if (b.in_pos == b.in_size && offset < length) {
      const uint32_t count = std::min<uint32_t>(IN_CHUNK, length - offset);
      if (flash_read(this->staging_flash_, start + offset, in, count) != count) {
        valid = false;
        break;
      }
      offset += count;
      b.in = in;
      b.in_pos = 0;
      b.in_size = count;
    }
    ret = xz_dec_run(dec, &b);
    if (b.out_pos != 0) {
      valid = check.add(out, b.out_pos);
      b.out_pos = 0;
    }
    // A truncated stream ends with XZ_BUF_ERROR: multi-call mode reports it on the second call without progress.
    App.feed_wdt();
  }
  xz_dec_end(dec);
  allocator.deallocate(in, IN_CHUNK + OUT_CHUNK);
  const bool ok = valid && ret == XZ_STREAM_END && offset == length && b.in_pos == b.in_size && check.complete();
  if (!ok) {
    ESP_LOGE(TAG, "Staged image check failed (xz=%d, in=%u/%u, out=%u, sections=%u)", static_cast<int>(ret),
             static_cast<unsigned>(offset), static_cast<unsigned>(length), static_cast<unsigned>(check.total()),
             static_cast<unsigned>(check.sections()));
    return false;
  }
  ESP_LOGI(TAG, "Staged image decodes to %u bytes in %u sections", static_cast<unsigned>(check.total()),
           static_cast<unsigned>(check.sections()));
  return true;
}

void XR806OTABackend::stop_() {
  if (this->sdk_initialized_) {
    ota_push_stop();
    this->sdk_initialized_ = false;
  } else {
    ota_deinit();
  }
  if (this->pm_held_) {
    if (pm_wake_unlock(&ota_native_wakelock) != 0) {
      ESP_LOGE(TAG, "Native OTA wakelock release failed");
    }
    this->pm_held_ = false;
  }
  // No permanent "busy" OTA state after an interrupted/invalid update.
  this->update_lock_.release();
}

void XR806OTABackend::abort() {
  if (this->committed_)
    return;
  if (this->sdk_initialized_ || this->pm_held_)
    this->stop_();
  // On cancellation/invalid image, leave FDCM selection unchanged.
  this->writing_ = false;
  this->received_ = 0;
  this->declared_ = 0;
  this->required_ = 0;
  this->preface_len_ = 0;
  this->md5_supplied_ = false;
  this->md5_valid_ = false;
}
}  // namespace esphome::ota
#endif  // USE_XR806
