// Portable Stage 8 race-state tests. No real WLAN/PM/sockets are emulated.
// g++ -std=c++17 -Wall -Wextra -Werror -pthread -I. tests/xr806/standby_service_lifecycle_test.cpp -o /tmp/life && /tmp/life
#include <cassert>
#include <atomic>
#include <cstdint>
#include <thread>
#include "esphome/components/xr806/standby_service_lifecycle.h"
#include "esphome/components/xr806/lowpower_policy.h"

int main() {
  using esphome::xr806::XR806StandbyServiceLifecycle;
  using esphome::xr806::XR806LowPowerPolicy;
  XR806StandbyServiceLifecycle lifecycle;
  assert(lifecycle.active() && !lifecycle.enter());
  assert(lifecycle.begin(100));
  assert(!lifecycle.begin(101) && lifecycle.preparing());
  assert(!lifecycle.ready(false));  // mDNS TCP/IP callback has not ACKed.
  assert(!lifecycle.expired(2099));
  assert(lifecycle.expired(2100));
  lifecycle.resume();
  lifecycle.restored();
  assert(lifecycle.active());

  assert(lifecycle.begin(3000));
  assert(lifecycle.ready(true) && lifecycle.quiesced());
  assert(lifecycle.enter());
  assert(lifecycle.paused());
  lifecycle.resume();
  lifecycle.restored();

  // Millis wrap must not make a pending mDNS ACK expire prematurely.
  assert(lifecycle.begin(0xFFFFFFF0u));
  assert(!lifecycle.expired(0x00000010u));
  assert(lifecycle.expired(static_cast<uint32_t>(0xFFFFFFF0u + 2000u)));
  lifecycle.resume();
  lifecycle.restored();

  XR806LowPowerPolicy p;
  p.require_external_mcu(true);
  p.request_standby(true);
  p.set_mcu_synchronized(true);
  p.set_uart_idle(true);
  p.set_scheduler_idle(true);
  p.set_wake_source_ready(true);
  p.set_network_idle(true);
  p.set_api_idle(true);
  p.set_ota_idle(true);
  p.set_mdns_idle(true);
  const auto initial = p.snapshot();
  assert(initial.stable && initial.blockers == 0 && p.can_enter_standby(initial.generation));

  // A live API client / new OTA / WLAN callback vetoes the prior vote epoch.
  p.set_api_idle(false);
  assert(!p.can_enter_standby(initial.generation));
  assert((p.snapshot().blockers & XR806LowPowerPolicy::API_BUSY) != 0);
  p.set_api_idle(true);
  p.set_ota_idle(false);
  assert(!p.can_enter_standby(initial.generation));
  p.set_ota_idle(true);
  p.set_network_idle(false);
  assert(!p.can_enter_standby());
  p.set_network_idle(true);
  const auto current = p.snapshot();
  assert(current.stable && current.blockers == 0);
  assert(p.can_enter_standby(current.generation));
  assert(!p.can_enter_standby(initial.generation));

  // The volatile owner writer may change votes on another task. The stable
  // snapshot must never be treated as an irrevocable admission: a newer
  // owner generation invalidates it before PM. This is not a physical IRQ
  // race proof; the native SDK wakelocks protect the remaining interval.
  std::atomic<bool> start{false};
  std::atomic<bool> writer_finished{false};
  std::thread radio([&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int i = 0; i < 10000; ++i) {
      p.set_network_idle(false);
      p.set_network_idle(true);
    }
    p.set_network_idle(false);
    writer_finished.store(true, std::memory_order_release);
  });
  start.store(true, std::memory_order_release);
  while (!writer_finished.load(std::memory_order_acquire)) {
    const auto snapshot = p.snapshot();
    if (snapshot.stable && snapshot.blockers == 0) {
      // This was an instantaneous snapshot, never a lockless lifetime lease.
      (void) p.can_enter_standby(snapshot.generation);
    }
  }
  radio.join();
  assert(!p.can_enter_standby());
  assert((p.snapshot().blockers & XR806LowPowerPolicy::NETWORK_BUSY) != 0);
  return 0;
}
