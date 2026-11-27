#include <gtest/gtest.h>
#include "esphome/components/tuya/tuya_low_power_standby_state.h"

namespace esphome::tuya::testing {

using State = TuyaLowPowerStandbyState;
using Action = State::ResumeAction;

TEST(TuyaLowPowerStandbyState, RequestNotificationAndCancel) {
  State s;
  EXPECT_TRUE(s.accept_request());
  EXPECT_FALSE(s.accept_request());  // A repeated CMD38 cannot restart the request
  EXPECT_TRUE(s.needs_notification());
  EXPECT_FALSE(s.ready_for_pm());
  EXPECT_FALSE(s.cancelled());  // No CMD12/01 sent yet: nothing to take back
  EXPECT_FALSE(s.requested());

  EXPECT_TRUE(s.accept_request());
  s.notification_sent();
  EXPECT_FALSE(s.needs_notification());
  EXPECT_FALSE(s.ready_for_pm());  // The attempt that sent CMD12/01 is declined on purpose
  EXPECT_FALSE(s.cancelled());
  EXPECT_TRUE(s.ready_for_pm());
  EXPECT_TRUE(s.cancelled());  // A later decline needs CMD12/00
  EXPECT_FALSE(s.requested() || s.ready_for_pm());

  EXPECT_TRUE(s.accept_request());
  s.notification_sent();
  EXPECT_TRUE(s.force_cancel());
  EXPECT_TRUE(s.accept_request());
  EXPECT_FALSE(s.force_cancel());  // Not notified yet
}

TEST(TuyaLowPowerStandbyState, ResumeActions) {
  State s;
  EXPECT_EQ(s.resumed(true, true), Action::RESUME_ACTION_NONE);  // No consent

  EXPECT_TRUE(s.accept_request());
  s.notification_sent();
  (void) s.cancelled();
  EXPECT_EQ(s.resumed(true, true), Action::RESUME_ACTION_SEND_WAKE);
  EXPECT_FALSE(s.recovery_required());
  EXPECT_TRUE(s.accept_request());  // A new handshake after a wake by the MCU

  s.notification_sent();
  (void) s.cancelled();
  EXPECT_EQ(s.resumed(true, false), Action::RESUME_ACTION_REQUIRE_RESYNC);  // A timer wake is no MCU wake
  EXPECT_TRUE(s.recovery_required());
  EXPECT_FALSE(s.accept_request());
  s.reset_on_mcu_boot();
  EXPECT_FALSE(s.recovery_required());
  EXPECT_TRUE(s.accept_request());

  s.notification_sent();
  (void) s.cancelled();
  EXPECT_EQ(s.resumed(false, true), Action::RESUME_ACTION_REQUIRE_RESYNC);  // An unexpected source
  s.recovered();
  EXPECT_TRUE(s.accept_request());
  EXPECT_EQ(s.resumed(true, true), Action::RESUME_ACTION_NONE);  // Requested, but CMD12/01 never sent
}

TEST(TuyaLowPowerStandbyState, KeepConsentWhileNetworkStaysConnected) {
  State s;
  EXPECT_TRUE(s.accept_request());
  s.notification_sent();
  (void) s.cancelled();
  EXPECT_TRUE(s.ready_for_pm());

  // A timer or received network data: the MCU still sleeps, so the module sleeps again without a new CMD38
  EXPECT_EQ(s.resumed(true, false, true), Action::RESUME_ACTION_KEEP_CONSENT);
  EXPECT_TRUE(s.requested() && s.ready_for_pm());
  EXPECT_FALSE(s.recovery_required());
  EXPECT_EQ(s.resumed(true, false, true), Action::RESUME_ACTION_KEEP_CONSENT);

  // The MCU's own wake still ends the consent with CMD12/00
  EXPECT_EQ(s.resumed(true, true, true), Action::RESUME_ACTION_SEND_WAKE);
  EXPECT_FALSE(s.requested());

  // An unexpected wake still needs a full status, also with the network kept up
  EXPECT_TRUE(s.accept_request());
  s.notification_sent();
  (void) s.cancelled();
  EXPECT_EQ(s.resumed(false, false, true), Action::RESUME_ACTION_REQUIRE_RESYNC);
  EXPECT_TRUE(s.recovery_required());

  // A write from the network ends the kept consent and needs CMD12/00
  s.reset_on_mcu_boot();
  EXPECT_TRUE(s.accept_request());
  s.notification_sent();
  (void) s.cancelled();
  EXPECT_EQ(s.resumed(true, false, true), Action::RESUME_ACTION_KEEP_CONSENT);
  EXPECT_TRUE(s.force_cancel());
  EXPECT_FALSE(s.requested());
}

TEST(TuyaLowPowerStandbyState, TransportFaultBlocksUntilRecovered) {
  State s;
  EXPECT_TRUE(s.accept_request());
  s.transport_fault();
  EXPECT_TRUE(s.recovery_required());
  EXPECT_FALSE(s.requested());
  EXPECT_FALSE(s.accept_request());
  EXPECT_FALSE(s.accept_offer());
  s.recovered();
  EXPECT_TRUE(s.accept_request());
}

TEST(TuyaLowPowerStandbyState, Version3OfferIsNotifiedConsent) {
  State s;
  EXPECT_TRUE(s.accept_offer());
  EXPECT_FALSE(s.accept_offer());
  EXPECT_FALSE(s.needs_notification());  // The 0x91 answer is the whole handshake
  EXPECT_TRUE(s.ready_for_pm());
  EXPECT_EQ(s.resumed(true, true), Action::RESUME_ACTION_SEND_WAKE);

  EXPECT_TRUE(s.accept_offer());
  EXPECT_TRUE(s.force_cancel());  // Cancelling tells the sleeping MCU that the module is awake
}

}  // namespace esphome::tuya::testing
