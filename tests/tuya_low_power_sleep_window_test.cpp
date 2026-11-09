// Host-only regression of newly emulated HC32 RTC/DP101/DP102 behavior.
// Does not claim physical PA3 reset, valid RTC wiring or PM entry.
#include <cassert>
#include <cstdint>
#include "esphome/components/tuya/tuya_low_power_sleep_window.h"
#include "esphome/components/tuya/tuya_low_power_product_retry.h"

int main() {
  using esphome::tuya::TuyaLowPowerSleepWindow;
  using esphome::tuya::TuyaLowPowerProductRetry;
  using Window = TuyaLowPowerSleepWindow;

  const uint8_t enabled[] = {1};
  const uint8_t disabled[] = {0};
  const uint8_t invalid_bool[] = {2};
  const uint8_t overnight[] = {22, 30, 6, 45};
  const uint8_t invalid_minute[] = {22, 60, 6, 45};
  const uint8_t invalid_hour[] = {24, 0, 6, 45};
  const uint8_t changed[] = {8, 0, 18, 0};
  assert(Window::valid_datapoint(101, 1, enabled, 1));
  assert(Window::valid_datapoint(101, 1, disabled, 1));
  assert(!Window::valid_datapoint(101, 1, invalid_bool, 1));
  assert(!Window::valid_datapoint(101, 0, enabled, 1));
  assert(!Window::valid_datapoint(101, 1, enabled, 0));
  assert(!Window::valid_datapoint(101, 1, nullptr, 1));
  assert(Window::valid_datapoint(102, 0, overnight, 4));
  assert(!Window::valid_datapoint(102, 0, overnight, 1));   // Original HC32 RAW1 OOB bug
  assert(!Window::valid_datapoint(102, 0, overnight, 3));
  assert(!Window::valid_datapoint(102, 0, overnight, 5));
  assert(!Window::valid_datapoint(102, 1, overnight, 4));
  assert(!Window::valid_datapoint(102, 0, invalid_minute, 4));
  assert(!Window::valid_datapoint(102, 0, invalid_hour, 4));
  assert(!Window::valid_datapoint(102, 0, nullptr, 4));

  // All 1440 minutes of each of the 3 cases run in original-code emulation.
  for (int minute = 0; minute < 1440; ++minute) {
    auto hour = static_cast<uint8_t>(minute / 60);
    auto min = static_cast<uint8_t>(minute % 60);
    assert(Window::rtc_marker(false, true, true, hour, min, 8, 0, 18, 0) ==
           (minute >= 8 * 60 && minute < 18 * 60));
    assert(Window::rtc_marker(false, true, true, hour, min, 22, 0, 6, 0) ==
           (minute >= 22 * 60 || minute < 6 * 60));
    assert(!Window::rtc_marker(true, true, true, hour, min, 8, 0, 8, 0));
  }
  assert(Window::rtc_marker(true, false, false, 9, 0, 8, 0, 18, 0));
  assert(!Window::rtc_marker(false, false, true, 9, 0, 8, 0, 18, 0));
  assert(!Window::rtc_marker(true, true, false, 9, 0, 8, 0, 18, 0));

  Window state;
  assert(!state.standby_mode_stable());  // Product alone is insufficient.
  state.product_validated();
  assert(!state.standby_mode_stable());  // Need DP101 from a validated full status report.
  state.observe_report(101, 1, disabled, 1);
  assert(state.standby_mode_stable());   // Dormant disabled; last sm was validated.
  state.observe_report(102, 0, overnight, 4);
  assert(state.standby_mode_stable());
  state.observe_report(102, 0, overnight, 4); // No difference => do not invalidate.
  assert(state.product_epoch_valid());
  state.observe_report(102, 0, changed, 4);
  assert(!state.product_epoch_valid()); // An MCU update can make sm stale.
  state.product_validated();
  assert(state.standby_mode_stable());
  state.observe_report(101, 1, enabled, 1);
  assert(!state.standby_mode_stable());  // RTC may dynamically enter sm0.
  state.product_validated();
  assert(!state.standby_mode_stable());  // Even a fresh sm1 can turn into sm0 by RTC.
  state.observe_report(101, 1, disabled, 1);
  assert(!state.standby_mode_stable());  // Cannot trust stale product sm after change.
  state.product_validated();
  assert(state.standby_mode_stable());
  state.on_write(102);
  assert(!state.standby_mode_stable());  // An ACK is not a new product epoch.
  state.product_validated();
  assert(state.standby_mode_stable());
  state.reset_reports();
  assert(!state.standby_mode_stable());  // MCU reboot/status epoch needs fresh DP101.

  TuyaLowPowerProductRetry retry;
  retry.begin(10);
  assert(!retry.due(509));
  assert(retry.due(510));
  for (int i = 0; i < TuyaLowPowerProductRetry::MAX_RETRIES; ++i) {
    const uint32_t at = static_cast<uint32_t>(510 + i * 500);
    assert(retry.due(at) && !retry.exhausted());
    retry.query_sent(at);
    assert(!retry.due(at + 499));
  }
  assert(retry.exhausted() && retry.due(3010));
  retry.success();
  assert(!retry.due(9000) && retry.attempts() == 0);
  retry.begin(0xFFFFFFF0u);
  assert(!retry.due(0x00000010u));
  assert(retry.due(static_cast<uint32_t>(0xFFFFFFF0u + 500u)));
  return 0;
}
