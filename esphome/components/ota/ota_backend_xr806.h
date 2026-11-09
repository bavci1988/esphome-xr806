#pragma once
#ifdef USE_XR806
#include "ota_backend.h"
#include "esphome/components/md5/md5.h"
#include "esphome/components/xr806/ota_writer_geometry.h"
#include "esphome/components/xr806/ota_writer_image.h"
#include <cstddef>
#include <cstdint>
#include <memory>
namespace esphome::ota {
class XR806OTABackend final {
 public:
  ~XR806OTABackend();
  OTAResponseTypes begin(size_t image_size, OTAType type = OTA_TYPE_UPDATE_APP);
  void set_update_md5(const char *md5);
  OTAResponseTypes write(uint8_t *data, size_t len);
  OTAResponseTypes end();
  void abort();
  // ESPHome gzip/deflate is not XRadio's XZ OTA container.
  static constexpr bool supports_compression() { return false; }

 private:
  bool push_(uint8_t *data, size_t bytes);
  bool probe_staged_image_();
  bool read_commit_();
  void stop_();
  UpdateLock update_lock_{};
  md5::MD5Digest digest_{};
  xr806::WriterPlan plan_{};
  uint8_t preface_[xr806::XR806_OTA_PREFACE_BYTES]{};
  uint8_t expected_md5_[16]{};
  uint32_t received_{0}, declared_{0}, required_{0};
  uint32_t image_base_{0};  // ROM bl_size: section next_addr values are relative to it
  uint8_t staging_flash_{0};
  size_t preface_len_{0};
  bool md5_supplied_{false}, md5_valid_{false};
  bool sdk_initialized_{false}, writing_{false}, pm_held_{false}, committed_{false};
};
std::unique_ptr<XR806OTABackend> make_ota_backend();
}  // namespace esphome::ota
#endif
