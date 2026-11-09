#include <gtest/gtest.h>
#include "esphome/components/tuya/tuya_low_power_standby_state.h"

namespace esphome::tuya::testing {

TEST(TuyaLowPowerStandbyState, ResumeActions) {
  using esphome::tuya::TuyaLowPowerStandbyState;
  using State = TuyaLowPowerStandbyState;
  using Action = State::ResumeAction;

  State s;
  EXPECT_FALSE(s.accept_request(false, true, true, true));  // sm=0 => hibernation, not retained standby
  EXPECT_FALSE(s.accept_request(true, false, true, true));  // unconfirmed DP write/status
  EXPECT_FALSE(s.accept_request(true, true, false, true));  // UART transport fault
  EXPECT_FALSE(s.accept_request(true, true, true, false));  // WLAN/API/OTA veto
  EXPECT_TRUE(s.accept_request(true, true, true, true));
  EXPECT_FALSE(s.accept_request(true, true, true, true));  // repeated CMD38 cannot restart request
  EXPECT_TRUE(s.needs_notification());
  EXPECT_FALSE(s.ready_for_pm());
  EXPECT_FALSE(s.cancelled());  // request without CMD12 needs no compensation
  EXPECT_FALSE(s.requested());

  EXPECT_TRUE(s.accept_request(true, true, true, true));
  s.notification_sent();
  EXPECT_FALSE(s.ready_for_pm());
  EXPECT_FALSE(s.cancelled());  // intentional retry after physical UART drain
  EXPECT_TRUE(s.ready_for_pm());
  EXPECT_TRUE(s.notified());
  EXPECT_TRUE(s.cancelled());  // veto after CMD12: must send compensating CMD12/00
  EXPECT_TRUE(!s.requested() && !s.notified());

  EXPECT_TRUE(s.accept_request(true, true, true, true));
  s.notification_sent();
  (void) s.cancelled();
  EXPECT_TRUE(s.resumed(true, true) == Action::SEND_CMD12_WAKE);
  EXPECT_FALSE(s.recovery_required());
  EXPECT_TRUE(s.accept_request(true, true, true, true));  // new handshake after GPIO wake

  s.notification_sent();
  (void) s.cancelled();
  EXPECT_TRUE(s.resumed(true, false) == Action::REQUIRE_RESYNC);  // timer alone never fakes GPIO wake
  EXPECT_TRUE(s.recovery_required());
  EXPECT_FALSE(s.accept_request(true, true, true, true));
  s.reset_on_mcu_boot();
  EXPECT_FALSE(s.recovery_required());
  EXPECT_TRUE(s.accept_request(true, true, true, true));

  s.notification_sent();
  (void) s.cancelled();
  EXPECT_TRUE(s.resumed(false, true) == Action::REQUIRE_RESYNC);
  s.reset_on_mcu_boot();
  EXPECT_TRUE(s.accept_request(true, true, true, true));
  s.transport_fault();
  EXPECT_TRUE(s.recovery_required() && !s.requested());
  EXPECT_FALSE(s.accept_request(true, true, true, true));

  State empty;
  EXPECT_TRUE(empty.resumed(true, true) == Action::NONE);
}

}  // namespace esphome::tuya::testing
