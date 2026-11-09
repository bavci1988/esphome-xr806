#include <cassert>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>
#include "esphome/components/xr806/ota_writer_geometry.h"
#include "esphome/components/xr806/ota_writer_image.h"
using namespace esphome::xr806;

namespace {

void put_le32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// Append an AWIH section at relative offset `at` (absolute address base + at).
void add_section(std::vector<uint8_t> &img, uint32_t at, uint32_t id, const std::vector<uint8_t> &body, uint32_t next) {
  if (img.size() < at + 64 + body.size())
    img.resize(at + 64 + body.size(), 0xFF);
  uint8_t *h = img.data() + at;
  std::memset(h, 0xFF, 64);
  put_le32(h, AWIH_MAGIC);
  put_le32(h + 4, AWIH_VERSION);
  h[8] = h[9] = 0;
  const uint16_t body_sum = static_cast<uint16_t>(0xFFFF - sum16(body.data(), body.size()));
  h[10] = static_cast<uint8_t>(body_sum);
  h[11] = static_cast<uint8_t>(body_sum >> 8);
  put_le32(h + 12, static_cast<uint32_t>(body.size()));
  put_le32(h + 24, static_cast<uint32_t>(body.size()));
  put_le32(h + 28, 1);
  put_le32(h + 32, next);
  put_le32(h + 36, id);
  const uint16_t head_sum = static_cast<uint16_t>(0xFFFF - sum16(h, 64));
  h[8] = static_cast<uint8_t>(head_sum);
  h[9] = static_cast<uint8_t>(head_sum >> 8);
  std::memcpy(h + 64, body.data(), body.size());
}

constexpr uint32_t BASE = 0x8000;

std::vector<uint8_t> two_section_image() {
  std::vector<uint8_t> app(4097), xip(1500);  // Odd length exercises the checksum byte pairing.
  for (size_t i = 0; i < app.size(); ++i)
    app[i] = static_cast<uint8_t>(i * 7 + 3);
  for (size_t i = 0; i < xip.size(); ++i)
    xip[i] = static_cast<uint8_t>(i * 13 + 1);
  std::vector<uint8_t> img;
  const uint32_t second = 0x2000;  // Leaves a padded gap after the first section.
  add_section(img, 0, AWIH_APP_ID, app, BASE + second);
  add_section(img, second, 0xA5FD5A02, xip, AWIH_NO_NEXT);
  return img;
}

bool check(const std::vector<uint8_t> &img, uint32_t limit, uint32_t seed) {
  DecompressedImageCheck c(BASE, limit);
  std::mt19937 rng(seed);
  size_t pos = 0;
  while (pos < img.size()) {
    const size_t n = std::min<size_t>(img.size() - pos, 1 + rng() % 700);
    if (!c.add(img.data() + pos, n))
      return false;
    pos += n;
  }
  return c.complete();
}

void test_decompressed_image_check() {
  const auto img = two_section_image();
  for (uint32_t seed = 1; seed < 20; ++seed)
    assert(check(img, 0x10000, seed));
  // Fed in one call as well.
  DecompressedImageCheck whole(BASE, 0x10000);
  assert(whole.add(img.data(), img.size()) && whole.complete() && whole.sections() == 2);

  // A flipped body byte in either section is caught.
  auto bad = img;
  bad[64 + 100] ^= 0x40;
  assert(!check(bad, 0x10000, 3));
  bad = img;
  bad[0x2000 + 64 + 9] ^= 1;
  assert(!check(bad, 0x10000, 3));
  // A corrupted header is caught.
  bad = img;
  bad[16] ^= 1;
  assert(!check(bad, 0x10000, 3));
  // A first section that is not APP is refused.
  bad = img;
  put_le32(bad.data() + 36, 0xA5FD5A02);
  assert(!check(bad, 0x10000, 3));
  // Truncated output (decoder stopped early) never completes.
  std::vector<uint8_t> cut(img.begin(), img.end() - 10);
  assert(!check(cut, 0x10000, 3));
  // Output larger than the active area is refused before it could reach FDCM/staging.
  assert(!check(img, static_cast<uint32_t>(img.size() - 1), 3));
  // next_addr pointing backwards into the previous section is refused.
  bad = img;
  put_le32(bad.data() + 32, BASE + 16);
  const uint16_t fix = static_cast<uint16_t>(0xFFFF - (sum16(bad.data(), 64) - (bad[8] | (bad[9] << 8))));
  bad[8] = static_cast<uint8_t>(fix);
  bad[9] = static_cast<uint8_t>(fix >> 8);
  assert(sum16(bad.data(), 64) == 0xFFFF);
  assert(!check(bad, 0x10000, 3));
}

void test_preface() {
  assert(!validate_xz_preface(nullptr, 0, 640u * 1024u, 0).valid);
  uint8_t p[XR806_OTA_PREFACE_BYTES]{};
  std::memset(p, 0xFF, 64);
  put_le32(p, AWIH_MAGIC);
  put_le32(p + 4, AWIH_VERSION);
  p[8] = p[9] = 0;
  put_le32(p + 12, 1000);
  put_le32(p + 24, 1000);
  put_le32(p + 28, 0x11);
  put_le32(p + 32, AWIH_NO_NEXT);
  put_le32(p + 36, AWIH_APP_ID);
  const uint16_t s = static_cast<uint16_t>(0xFFFF - sum16(p, 64));
  p[8] = static_cast<uint8_t>(s);
  p[9] = static_cast<uint8_t>(s >> 8);
  const uint8_t xz[6] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};
  std::memcpy(p + 64, xz, 6);
  const auto ok = validate_xz_preface(p, sizeof(p), 640u * 1024u, 1064);
  assert(ok.valid && ok.bytes == 1064);
  assert(!validate_xz_preface(p, sizeof(p), 640u * 1024u, 1065).valid);  // Declared size mismatch.
  assert(!validate_xz_preface(p, sizeof(p), 900, 0).valid);              // Larger than the slot.
  p[64] = 0;
  assert(!validate_xz_preface(p, sizeof(p), 640u * 1024u, 0).valid);  // Not an XZ stream.
}

}  // namespace

int main() {
  // 2 MiB WXU layout from xr806-sdk project/demo/trv16_port/image/xr806/image.cfg.
  WriterGeometry g{};
  g.flash_size = 2u * 1024u * 1024u;
  g.metadata_addr = 1276u * 1024u;
  g.metadata_size = 4096;
  g.image_addr[0] = 32768;
  g.image_addr[1] = 1280u * 1024u;
  g.image_max_kib = 1244;
  g.compressed_max_kib = 640;
  g.bootloader_bytes = 32768;
  g.running_seq = 0;
  g.selected_seq = 0;
  g.selected_verified = true;
  g.upload_bytes = 600u * 1024u;
  auto result = validate_writer(g);
  assert(result.error == WriterError::OK);
  assert(result.candidate_seq == 1);
  assert(result.candidate.start == 1280u * 1024u);
  assert(result.candidate.size == 640u * 1024u);
  // Native updater rejects oversized files rather than writing across flash.
  g.upload_bytes = 641u * 1024u;
  assert(validate_writer(g).error == WriterError::SIZE);
  g.upload_bytes = 0;  // web upload with unknown length
  g.metadata_addr = g.image_addr[1];
  assert(validate_writer(g).error == WriterError::OVERLAP);
  g.metadata_addr = 1276u * 1024u;
  g.running_seq = 1;  // seq 1 is compressed staging, never a running writer
  g.selected_seq = 1;
  assert(validate_writer(g).error == WriterError::SEQUENCE);
  g.running_seq = 0;
  g.selected_seq = 1;  // a committed update waits for the bootloader: no second upload
  assert(validate_writer(g).error == WriterError::SEQUENCE);
  g.selected_seq = 0;
  g.selected_verified = false;
  assert(validate_writer(g).error == WriterError::SEQUENCE);
  g.selected_verified = true;
  g.flash_id[1] = 1;
  assert(validate_writer(g).error == WriterError::FLASH);
  test_preface();
  test_decompressed_image_check();
  return 0;
}
