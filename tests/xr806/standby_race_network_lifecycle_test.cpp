#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include "esphome/components/xr806/lowpower_policy.h"
#include "esphome/components/xr806/standby_network_lifecycle.h"

int main() {
  using Policy = esphome::xr806::XR806LowPowerPolicy;
  using Network = esphome::xr806::XR806StandbyNetworkLifecycle;

  Network n;
  n.configure(Network::WLAN | Network::API | Network::OTA | Network::MDNS);
  const auto generation = n.generation();
  n.begin(generation);
  assert(!n.ready(generation));
  assert(n.pending() == (Network::WLAN | Network::API | Network::OTA | Network::MDNS));
  assert(n.owner_quiesced(Network::API, generation));
  assert(n.owner_quiesced(Network::MDNS, generation));
  assert(n.owner_quiesced(Network::OTA, generation));
  assert(!n.ready(generation));  // cannot skip vendor WLAN suspend/PM lock.
  assert(n.owner_quiesced(Network::WLAN, generation));
  assert(n.ready(generation));
  n.activity(Network::API);
  assert(!n.ready(generation));
  assert(!n.owner_quiesced(Network::API, generation));
  const auto epoch2 = n.generation();
  n.begin(epoch2);
  assert(n.owner_quiesced(Network::WLAN, epoch2));
  assert(n.owner_quiesced(Network::API, epoch2));
  assert(n.owner_quiesced(Network::OTA, epoch2));
  assert(n.owner_quiesced(Network::MDNS, epoch2));
  assert(n.ready(epoch2));
  n.resume();
  assert(!n.ready(epoch2));

  Policy p;
  p.set_uart_idle(true);
  p.set_network_idle(true);
  p.set_api_idle(true);
  p.set_ota_idle(true);
  p.set_mdns_idle(true);
  p.set_mcu_synchronized(true);
  p.request_standby(true);
  p.set_wake_source_ready(true);
  p.set_scheduler_idle(true);
  assert(p.can_enter_standby());
  const uint32_t epoch = p.generation();
  assert(p.begin_commit(epoch));
  assert(p.commit_valid(epoch));
  assert(!p.begin_commit(epoch));  // One PM entrant only.
  p.set_api_idle(false);
  assert(!p.commit_valid(epoch));  // Fresh client immediately vetoes.
  p.end_commit();
  p.set_api_idle(true);
  const uint32_t epoch3 = p.generation();
  assert(!p.commit_valid(epoch3));
  assert(p.begin_commit(epoch3));
  p.set_network_quiescing(true);
  assert(!p.commit_valid(epoch3));
  p.end_commit();
  p.set_network_quiescing(false);
  assert(p.begin_commit(p.generation()));
  p.request_standby(false);
  assert(!p.commit_valid(p.generation()));
  p.end_commit();
  assert(!p.can_enter_standby());

  // Exhaust each relative order of API close, mDNS drain, WLAN park,
  // post-prepare RX/OTA activity and final commit attempt. No transient
  // "all quiet" appearance may admit PM after an owner returns busy.
  std::array<int, 5> order{0, 1, 2, 3, 4};
  unsigned permutations = 0;
  do {
    Policy q;
    q.set_uart_idle(true);
    q.set_mcu_synchronized(true);
    q.set_ota_idle(true);
    q.request_standby(true);
    q.set_scheduler_idle(true);
    q.set_wake_source_ready(true);
    bool vetoed = false;
    for (int event : order) {
      switch (event) {
        case 0:
          q.set_api_idle(true);
          break;
        case 1:
          q.set_mdns_idle(true);
          break;
        case 2:
          q.set_network_idle(true);
          break;
        case 3:
          q.set_uart_idle(false);
          vetoed = true;
          break;
        case 4: {
          const auto e = q.generation();
          if (q.begin_commit(e)) {
            assert(!vetoed);  // Traffic never permits a PM admission.
            q.end_commit();
          }
          break;
        }
      }
    }
    assert(!q.can_enter_standby());  // UART RX stays busy for remainder.
    ++permutations;
  } while (std::next_permutation(order.begin(), order.end()));
  assert(permutations == 120);
  return 0;
}
