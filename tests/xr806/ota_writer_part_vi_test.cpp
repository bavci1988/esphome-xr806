#include <cassert>
#include <cstdint>
#include "esphome/components/xr806/ota_writer_geometry.h"
#include "esphome/components/xr806/ota_writer_image.h"
using namespace esphome::xr806;
int main() {
  WriterGeometry g{};
  g.flash_size = 2u * 1024u * 1024u;
  g.metadata_addr = 1276u * 1024u;
  g.metadata_size = 4096;
  g.image_addr[0] = 32768;
  g.image_addr[1] = 1280u * 1024u;
  g.image_max_kib = 1244;
  g.compressed_max_kib = 768;
  g.bootloader_bytes = 32768;
  g.running_seq = 0;
  g.selected_seq = 0;
  g.selected_verified = true;
  g.upload_bytes = 760u * 1024u;
  auto result = validate_writer(g);
  assert(result.error == WriterError::OK);
  assert(result.candidate_seq == 1);
  assert(result.candidate.start == 1280u * 1024u);
  assert(result.candidate.size == 768u * 1024u);
  // Native updater rejects oversized files rather than writing across flash.
  g.upload_bytes = 769u * 1024u;
  assert(validate_writer(g).error == WriterError::SIZE);
  g.upload_bytes = 0; // web upload with unknown length
  g.metadata_addr = g.image_addr[1];
  assert(validate_writer(g).error == WriterError::OVERLAP);
  g.metadata_addr = 1276u * 1024u;
  g.running_seq = 1; // seq 1 is compressed staging, never a running writer
  g.selected_seq = 1;
  assert(validate_writer(g).error == WriterError::SEQUENCE);
  g.running_seq = 0;
  g.selected_seq = 0;
  g.selected_verified = false;
  assert(validate_writer(g).error == WriterError::SEQUENCE);
  g.selected_verified = true;
  g.flash_id[1] = 1;
  assert(validate_writer(g).error == WriterError::FLASH);
  assert(!validate_xz_preface(nullptr, 0, 768u * 1024u, 0).valid);
  return 0;
}
