#include <cassert>
#include <cstdint>
#include "esphome/components/xr806/lowpower_policy.h"
#include "esphome/components/xr806/wifi_power_save_retry.h"

int main() {
  using esphome::xr806::XR806LowPowerPolicy;
  using esphome::xr806::XR806WiFiPowerSaveRetry;

  XR806LowPowerPolicy p;
  assert((p.blockers() & XR806LowPowerPolicy::MCU_UNSYNCED) == 0);
  assert((p.blockers() & XR806LowPowerPolicy::NOT_REQUESTED) != 0);
  p.request_standby(true);
  assert(!p.can_enter_standby());
  p.set_uart_idle(true);
  p.set_network_idle(true);
  p.set_ota_idle(true);
  p.set_api_idle(true);
  p.set_mdns_idle(true);
  p.set_wake_source_ready(true);
  p.set_scheduler_idle(true);
  assert(p.can_enter_standby());  // Generic WXU without an external MCU.

  p.require_external_mcu(true);
  assert((p.blockers() & XR806LowPowerPolicy::MCU_UNSYNCED) != 0);
  assert(!p.can_enter_standby());
  p.set_mcu_synchronized(true);
  assert(p.can_enter_standby());
  p.require_external_mcu(false);
  assert(p.can_enter_standby());
  p.require_external_mcu(true);  // Reconnection/driver re-registration fail closed.
  assert(!p.can_enter_standby());
  p.require_external_mcu(false);
  assert(p.can_enter_standby());

  p.set_network_idle(false);
  assert(!p.can_enter_standby());
  p.set_network_idle(true);
  p.set_api_idle(false);
  assert(!p.can_enter_standby());
  p.set_api_idle(true);
  p.set_mdns_idle(false);
  assert(!p.can_enter_standby());
  p.set_mdns_idle(true);
  p.set_ota_idle(false);
  assert(!p.can_enter_standby());
  p.set_ota_idle(true);
  assert(p.can_enter_standby());

  XR806WiFiPowerSaveRetry retry;
  assert(retry.due(100));
  retry.record(100, false);
  assert(!retry.due(101));
  assert(!retry.due(30099));
  assert(retry.due(30100));
  retry.record(30100, true);
  assert(retry.applied());
  assert(!retry.due(999999));
  retry.reset();
  assert(!retry.applied());
  assert(retry.due(999999));
  retry.record(0xfffffff0u, false);
  assert(!retry.due(0x10u));  // 32-bit millisecond clock wrap.
  assert(retry.due(static_cast<uint32_t>(0xfffffff0u + 30000u)));
  return 0;
}
