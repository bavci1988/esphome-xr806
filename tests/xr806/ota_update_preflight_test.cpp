#include <cassert>
#include <cstdint>
#include "esphome/components/xr806/ota_update_pm_gate.h"
#include "esphome/components/xr806/ota_flash_plan.h"

using esphome::xr806::XR806OTAUpdatePMGate;
using esphome::xr806::XR806OTAFlashPlan;
using esphome::xr806::XR806OTAFlashPlanError;
using esphome::xr806::xr806_validate_ota_flash_plan;

struct PMMock {
  bool lock_ok{true};
  bool unlock_ok{true};
  uint32_t veto_count{0};
  uint32_t lock_count{0};
  uint32_t unlock_count{0};
};
void veto(void *ctx) { ++static_cast<PMMock *>(ctx)->veto_count; }
bool lock(void *ctx) {
  auto &m = *static_cast<PMMock *>(ctx);
  ++m.lock_count;
  return m.lock_ok;
}
bool unlock(void *ctx) {
  auto &m = *static_cast<PMMock *>(ctx);
  ++m.unlock_count;
  return m.unlock_ok;
}

int main() {
  PMMock m{};
  XR806OTAUpdatePMGate g;
  assert(g.state() == XR806OTAUpdatePMGate::State::IDLE);
  assert(g.acquire(veto, lock, &m));
  assert(m.veto_count == 1 && m.lock_count == 1);
  assert(!g.acquire(veto, lock, &m));  // OTA writer cannot overlap itself.
  assert(g.release(veto, unlock, &m));
  assert(m.veto_count == 2 && m.unlock_count == 1);
  assert(!g.release(veto, unlock, &m));
  assert(g.acquire(veto, lock, &m) && g.release(veto, unlock, &m));
  PMMock failed_lock{};
  failed_lock.lock_ok = false;
  XR806OTAUpdatePMGate bad;
  assert(!bad.acquire(veto, lock, &failed_lock));
  assert(bad.faulted() && !bad.acquire(veto, lock, &failed_lock));
  PMMock failed_unlock{};
  failed_unlock.unlock_ok = false;
  XR806OTAUpdatePMGate poison;
  assert(poison.acquire(veto, lock, &failed_unlock));
  assert(!poison.release(veto, unlock, &failed_unlock));
  assert(poison.faulted() && !poison.acquire(veto, lock, &failed_unlock));

  // Hypothetical separately verified 2-MiB layout, NOT the TRV16 dump.
  XR806OTAFlashPlan p{2U * 1024U * 1024U,
                      {0x000000U, 0x0FF000U},
                      {0x101000U, 0x96000U},
                      {0x100000U, 0x1000U},
                      0x55000U,
                      false,
                      false,
                      false};
  assert(xr806_validate_ota_flash_plan(p) == XR806OTAFlashPlanError::UNVERIFIED_LAYOUT);
  p.layout_verified = p.image_format_verified = p.boot_recovery_verified = true;
  assert(xr806_validate_ota_flash_plan(p) == XR806OTAFlashPlanError::OK);
  p.image_bytes = 0x97000U;
  assert(xr806_validate_ota_flash_plan(p) == XR806OTAFlashPlanError::INVALID_IMAGE_SIZE);
  p.image_bytes = 0x55000U;
  p.running_image.length = 0x133000U;  // TRV16 dump has data beyond 0x100000.
  assert(xr806_validate_ota_flash_plan(p) == XR806OTAFlashPlanError::ACTIVE_IMAGE_OVERLAP);
  p.running_image.length = 0x0FF000U;
  p.candidate_image.offset = 0x100000U;
  assert(xr806_validate_ota_flash_plan(p) == XR806OTAFlashPlanError::METADATA_OVERLAP);
  p.candidate_image.offset = 0x101001U;
  assert(xr806_validate_ota_flash_plan(p) == XR806OTAFlashPlanError::INVALID_GEOMETRY);
  p.candidate_image.offset = 0x1ff000U;
  assert(xr806_validate_ota_flash_plan(p) == XR806OTAFlashPlanError::INVALID_GEOMETRY);
  // Original WXU BOOT header says metadata at 0x177000, 4KiB,
  // xz_max=400KiB, and the second AWIH sequence starts at 0x178000.
  // That identifies candidate geometry, NOT active-slot or recovery proof.
  XR806OTAFlashPlan original{2U * 1024U * 1024U,
                             {0x000000U, 0x131000U},  // Conservative span of first AWIH section series.
                             {0x178000U, 0x64000U},   // Candidate compressed update slot.
                             {0x177000U, 0x1000U},
                             0x50000U,
                             false,
                             false,
                             false};
  assert(xr806_validate_ota_flash_plan(original) == XR806OTAFlashPlanError::UNVERIFIED_LAYOUT);
  original.image_bytes = 0x65000U;
  assert(xr806_validate_ota_flash_plan(original) == XR806OTAFlashPlanError::INVALID_IMAGE_SIZE);
  // Deliberately never calls HAL_Flash_Open/Erase/Write or ota_push_start.
  return 0;
}
