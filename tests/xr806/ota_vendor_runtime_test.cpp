#include <cassert>
#include <cstdint>
#include "esphome/components/xr806/ota_vendor_runtime_preflight.h"
#include "esphome/components/xr806/ota_dry_run_session.h"

using namespace esphome::xr806;

static XR806OTARuntimeGeometry example() {
  XR806OTARuntimeGeometry g{};
  g.flash_bytes = 2 * 1024 * 1024;
  g.metadata_addr = 0x0f0000;
  g.metadata_bytes = 0x1000;
  g.image_addr[0] = 0x10000;
  g.image_addr[1] = 0x100000;
  g.image_flash[0] = g.image_flash[1] = 0xff;
  g.metadata_flash = 0xff;
  g.image_max_kib = 512;
  g.compressed_max_kib = 256;
  g.bootloader_bytes = 0x8000;
  g.requested_image_bytes = 16000;
  g.running_seq = 0;
  g.fdcm_selected_seq = 0;
  g.compression_policy = true;
  g.runtime_snapshot_observed = true;
  g.fdcm_state_verified = true;
  return g;
}

int main() {
  XR806OTARuntimeGeometry g = example();
  XR806OTARuntimeProof proof{true, true};
  auto result = xr806_preflight_runtime_ota(g, proof);
  assert(result.error == XR806OTARuntimeError::OK);
  assert(!result.can_erase);  // Read-only result, even after all proof flags.
  assert(result.candidate_seq == 1 && result.candidate.offset == 0x100000);
  assert(result.candidate.length == 256 * 1024);
  assert(result.running.offset == 0x10000);

  auto c = g;
  c.runtime_snapshot_observed = false;
  assert(xr806_preflight_runtime_ota(c, proof).error == XR806OTARuntimeError::SNAPSHOT_NOT_OBSERVED);
  c = g;
  c.running_seq = 2;
  assert(xr806_preflight_runtime_ota(c, proof).error == XR806OTARuntimeError::INVALID_SEQUENCE);
  c = g;
  c.fdcm_selected_seq = 1;
  assert(xr806_preflight_runtime_ota(c, proof).error == XR806OTARuntimeError::SELECTION_MISMATCH);
  c = g;
  c.fdcm_state_verified = false;
  assert(xr806_preflight_runtime_ota(c, proof).error == XR806OTARuntimeError::FDCM_UNVERIFIED);
  c = g;
  c.image_flash[1] = 0x23;
  assert(xr806_preflight_runtime_ota(c, proof).error == XR806OTARuntimeError::FLASH_ID_UNSUPPORTED);
  c = g;
  c.metadata_flash = 0x23;
  assert(xr806_preflight_runtime_ota(c, proof).error == XR806OTARuntimeError::FLASH_ID_UNSUPPORTED);
  c = g;
  c.image_addr[1] = 0x108001;
  assert(xr806_preflight_runtime_ota(c, proof).error == XR806OTARuntimeError::INVALID_GEOMETRY);
  c = g;
  c.requested_image_bytes = 257 * 1024;
  assert(xr806_preflight_runtime_ota(c, proof).error == XR806OTARuntimeError::EXCEEDS_SLOT);
  c = g;
  c.image_addr[1] = 0x80000;
  assert(xr806_preflight_runtime_ota(c, proof).error == XR806OTARuntimeError::RUNNING_IMAGE_OVERLAP);
  c = g;
  c.metadata_addr = 0x100000;
  assert(xr806_preflight_runtime_ota(c, proof).error == XR806OTARuntimeError::METADATA_OVERLAP);
  c = g;
  assert(xr806_preflight_runtime_ota(c, {false, true}).error == XR806OTARuntimeError::IMAGE_FORMAT_UNVERIFIED);
  assert(xr806_preflight_runtime_ota(c, {true, false}).error == XR806OTARuntimeError::BOOT_RECOVERY_UNVERIFIED);

  // The observed TRV16 dump's nominal 1496KiB active capacity, combined
  // with a presumed 0x8000 active start, would overlap the compressed slot.
  // We cannot authorise an erase based only on static boot-header values.
  c = g;
  c.metadata_addr = 0x177000;
  c.image_addr[0] = 0x8000;
  c.image_addr[1] = 0x178000;
  c.image_max_kib = 1496;
  c.compressed_max_kib = 400;
  assert(xr806_preflight_runtime_ota(c, proof).error == XR806OTARuntimeError::RUNNING_IMAGE_OVERLAP);

  // Model-only OTA flow, no ota_push_start/write/finish/ROM APIs.
  XR806OTADryRunSession model;
  assert(model.begin(result, 16000, 100));
  assert(!model.may_publish_boot_selection());
  assert(model.receive(8192, 110));
  assert(model.receive(7808, 150));
  assert(model.total_bytes() == 16000);
  assert(model.finalize(true, 160));
  assert(model.phase() == XR806OTADryRunSession::Phase::COMPLETE);
  assert(!model.may_publish_boot_selection());

  XR806OTADryRunSession invalid;
  auto bad = result;
  bad.error = XR806OTARuntimeError::IMAGE_FORMAT_UNVERIFIED;
  assert(!invalid.begin(bad, 16000, 100));
  assert(invalid.failure() == XR806OTADryRunSession::Failure::NOT_ADMITTED);

  XR806OTADryRunSession interrupted;
  assert(interrupted.begin(result, 16000, 100));
  assert(interrupted.receive(500, 120));
  interrupted.power_loss();
  assert(interrupted.phase() == XR806OTADryRunSession::Phase::FAILED);
  assert(interrupted.total_bytes() == 0);
  assert(interrupted.failure() == XR806OTADryRunSession::Failure::POWER_LOSS);
  assert(!interrupted.finalize(true, 200));

  XR806OTADryRunSession overflow;
  assert(overflow.begin(result, 16000, 0));
  assert(!overflow.receive(8193, 2));
  assert(overflow.failure() == XR806OTADryRunSession::Failure::INVALID_CHUNK);

  XR806OTADryRunSession too_many;
  assert(too_many.begin(result, 100, 0));
  assert(!too_many.receive(101, 2));
  assert(too_many.failure() == XR806OTADryRunSession::Failure::CAPACITY);

  XR806OTADryRunSession too_few;
  assert(too_few.begin(result, 16000, 0));
  assert(too_few.receive(100, 2));
  assert(!too_few.finalize(true, 4));
  assert(too_few.failure() == XR806OTADryRunSession::Failure::BAD_PHASE);

  XR806OTADryRunSession invalid_checksum;
  assert(invalid_checksum.begin(result, 16000, 0));
  assert(invalid_checksum.receive(8000, 1) && invalid_checksum.receive(8000, 2));
  assert(!invalid_checksum.finalize(false, 3));
  assert(invalid_checksum.failure() == XR806OTADryRunSession::Failure::IMAGE_VERIFY);

  XR806OTADryRunSession timeout;
  assert(timeout.begin(result, 16000, 0xfffffff0u));
  assert(!timeout.expired(0x20u));
  assert(!timeout.receive(1, static_cast<uint32_t>(0xfffffff0u + XR806OTADryRunSession::TRANSFER_TIMEOUT_MS)));
  assert(timeout.failure() == XR806OTADryRunSession::Failure::TIMEOUT);

  XR806OTADryRunSession abort;
  assert(abort.begin(result, 16000, 100));
  abort.abort();
  assert(abort.phase() == XR806OTADryRunSession::Phase::ABORTED);
  assert(!abort.receive(100, 200));
  assert(!abort.may_publish_boot_selection());
  return 0;
}
