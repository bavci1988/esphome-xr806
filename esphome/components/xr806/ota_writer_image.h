#pragma once
// Validate the AWIH/XZ wrapper before ota_push_start() erases the target slot, and the
// decompressed image before the boot selection is committed.
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace esphome::xr806 {
constexpr size_t XR806_OTA_PREFACE_BYTES = 70;  // 64-byte AWIH + 6-byte XZ magic
constexpr size_t AWIH_HEADER_BYTES = 64;
constexpr uint32_t AWIH_MAGIC = 0x48495741;
constexpr uint32_t AWIH_VERSION = 5;
constexpr uint32_t AWIH_APP_ID = 0xA5FE5A01;
constexpr uint32_t AWIH_COMPRESS = 1u << 4;
constexpr uint32_t AWIH_NO_NEXT = 0xFFFFFFFFu;
// project/bootloader/main.c: BL_DEC_IMG_DICT_MAX; the boot-time decoder rejects larger dictionaries.
constexpr uint32_t XR806_BOOTLOADER_XZ_DICT = 8 * 1024;
inline uint32_t load_le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline uint16_t sum16(const uint8_t *p, size_t n) {
  uint16_t sum = 0;
  for (size_t i = 0; i < n; i += 2)
    sum = static_cast<uint16_t>(sum + p[i] + (i + 1 < n ? static_cast<uint16_t>(p[i + 1]) << 8 : 0));
  return sum;
}
struct XZImageInfo {
  bool valid{false};
  uint32_t bytes{0};
};
inline XZImageInfo validate_xz_preface(const uint8_t *p, size_t n, uint32_t slot, uint32_t expected) {
  if (p == nullptr || n < XR806_OTA_PREFACE_BYTES || slot < XR806_OTA_PREFACE_BYTES)
    return {};
  const uint32_t body = load_le32(p + 12);
  const uint32_t flags = load_le32(p + 28);
  if (load_le32(p) != AWIH_MAGIC || load_le32(p + 4) != AWIH_VERSION || sum16(p, 64) != 0xFFFF ||
      load_le32(p + 36) != AWIH_APP_ID || (flags & AWIH_COMPRESS) == 0 || (flags & ((1u << 2) | (1u << 3))) != 0 ||
      load_le32(p + 32) != AWIH_NO_NEXT || body < 6 || load_le32(p + 24) != body || body > slot - 64 ||
      (expected != 0 && expected != body + 64))
    return {};
  constexpr uint8_t xz_magic[6] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};
  for (size_t i = 0; i < 6; ++i) {
    if (p[64 + i] != xz_magic[i])
      return {};
  }
  return {true, body + 64};
}

/// Checks the bytes the bootloader will write into the active image area, as they stream
/// out of the decompressor. The bootloader erases that area first and, after a decode
/// error, still marks the image verified (it only re-checks the compressed wrapper), so
/// this must pass before the staged update is committed.
///
/// Mirrors the ROM section walk: AWIH sections start with an APP section at offset 0 and
/// follow next_addr (absolute, so relative to `base`) until next_addr is 0xFFFFFFFF. Each
/// header and body checksum must be 0xFFFF. Bytes between sections are not checked.
class DecompressedImageCheck {
 public:
  DecompressedImageCheck(uint32_t base, uint32_t limit) : base_(base), limit_(limit) {}

  /// Feed the next chunk. Returns false once the image is known to be invalid.
  bool add(const uint8_t *data, size_t len) {
    if (this->failed_)
      return false;
    if (len > this->limit_ - this->total_)
      return this->fail_();  // Would overrun the active image area into FDCM/staging.
    for (size_t i = 0; i < len && !this->failed_; ++i)
      this->byte_(data[i], this->total_ + static_cast<uint32_t>(i));
    this->total_ += static_cast<uint32_t>(len);
    return !this->failed_;
  }

  /// True when the whole section chain was seen and every checksum matched.
  bool complete() const { return !this->failed_ && this->done_; }
  uint32_t total() const { return this->total_; }
  uint32_t sections() const { return this->sections_; }

 protected:
  enum class Phase : uint8_t { GAP, HEADER, BODY, DONE };

  bool fail_() {
    this->failed_ = true;
    return false;
  }

  void byte_(uint8_t b, uint32_t pos) {
    switch (this->phase_) {
      case Phase::GAP:
        if (pos != this->next_start_)
          return;
        this->phase_ = Phase::HEADER;
        this->header_len_ = 0;
        [[fallthrough]];
      case Phase::HEADER:
        this->header_[this->header_len_++] = b;
        if (this->header_len_ == AWIH_HEADER_BYTES)
          this->header_done_(pos);
        return;
      case Phase::BODY:
        this->body_sum_ = static_cast<uint16_t>(this->body_sum_ + (this->odd_ ? (b << 8) : b));
        this->odd_ = !this->odd_;
        if (--this->body_left_ == 0)
          this->body_done_();
        return;
      case Phase::DONE:
        return;
    }
  }

  void header_done_(uint32_t pos) {
    const uint8_t *h = this->header_;
    if (load_le32(h) != AWIH_MAGIC || load_le32(h + 4) != AWIH_VERSION || sum16(h, AWIH_HEADER_BYTES) != 0xFFFF ||
        (this->sections_ == 0 && load_le32(h + 36) != AWIH_APP_ID) || this->sections_ >= 32) {
      this->fail_();
      return;
    }
    ++this->sections_;
    this->body_sum_ = static_cast<uint16_t>(h[10] | (h[11] << 8));  // data_chksum
    this->body_left_ = load_le32(h + 12);                           // data_size
    this->next_ = load_le32(h + 32);
    this->body_end_ = static_cast<uint64_t>(pos) + 1 + this->body_left_;
    this->odd_ = false;
    if (this->body_left_ == 0 || this->body_end_ > this->limit_) {
      this->fail_();
      return;
    }
    this->phase_ = Phase::BODY;
  }

  void body_done_() {
    if (this->body_sum_ != 0xFFFF) {
      this->fail_();
      return;
    }
    if (this->next_ == AWIH_NO_NEXT) {
      this->phase_ = Phase::DONE;
      this->done_ = true;
      return;
    }
    if (this->next_ < this->base_ || static_cast<uint64_t>(this->next_) - this->base_ < this->body_end_ ||
        this->next_ - this->base_ >= this->limit_) {
      this->fail_();
      return;
    }
    this->next_start_ = this->next_ - this->base_;
    this->phase_ = Phase::GAP;
  }

  uint8_t header_[AWIH_HEADER_BYTES]{};
  uint64_t body_end_{0};
  uint32_t base_;
  uint32_t limit_;
  uint32_t total_{0};
  uint32_t next_start_{0};
  uint32_t body_left_{0};
  uint32_t next_{0};
  uint32_t sections_{0};
  uint16_t body_sum_{0};
  uint8_t header_len_{0};
  Phase phase_{Phase::GAP};
  bool odd_{false};
  bool failed_{false};
  bool done_{false};
};
}  // namespace esphome::xr806
