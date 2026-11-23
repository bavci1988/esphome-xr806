#include <gtest/gtest.h>
#include "esphome/components/xr806/standby_coordinator_state.h"

namespace esphome::xr806::testing {

static constexpr uint32_t QUIET_MS = 250;
static constexpr uint32_t AWAKE_MS = 5000;

// Runs due() until the quiet interval has passed at `now`, with the generation kept constant.
static bool due_after_quiet(XR806StandbyCoordinatorState &state, uint32_t now) {
  state.due(now, 1, true, true);
  return state.due(now + QUIET_MS, 1, true, true);
}

TEST(XR806StandbyCoordinatorState, UnexpectedWakeDoublesAwakeHoldUpToMaximum) {
  XR806StandbyCoordinatorState state;
  state.configure(QUIET_MS, AWAKE_MS);
  state.boot(0);
  EXPECT_EQ(state.awake_hold_ms(), AWAKE_MS);
  state.finished(0, false);
  EXPECT_EQ(state.awake_hold_ms(), 2 * AWAKE_MS);
  state.finished(0, false);
  EXPECT_EQ(state.awake_hold_ms(), 4 * AWAKE_MS);
  for (int i = 0; i < 20; ++i)
    state.finished(0, false);
  EXPECT_EQ(state.awake_hold_ms(), XR806StandbyCoordinatorState::MAX_BACKOFF_MS);
  // Never disabled: an attempt is due again after the longest hold
  EXPECT_TRUE(due_after_quiet(state, XR806StandbyCoordinatorState::MAX_BACKOFF_MS));
  state.finished(0, true);
  EXPECT_EQ(state.failures(), 0);
  EXPECT_EQ(state.awake_hold_ms(), AWAKE_MS);
}

TEST(XR806StandbyCoordinatorState, NotEnteredIsNoFailure) {
  XR806StandbyCoordinatorState state;
  state.configure(QUIET_MS, AWAKE_MS);
  state.boot(0);
  state.finished(0, false);
  state.not_entered(10000);
  EXPECT_EQ(state.failures(), 1);
  // A request is consumed and the awake hold starts again at the time of the attempt
  EXPECT_FALSE(due_after_quiet(state, 10000 + AWAKE_MS));
  EXPECT_TRUE(due_after_quiet(state, 10000 + 2 * AWAKE_MS));
}

TEST(XR806StandbyCoordinatorState, DueNeedsRequestOwnersAndStableGeneration) {
  XR806StandbyCoordinatorState state;
  state.configure(QUIET_MS, AWAKE_MS);
  state.boot(0);
  EXPECT_FALSE(state.due(AWAKE_MS, 1, false, true));
  EXPECT_FALSE(state.due(AWAKE_MS, 1, true, false));
  EXPECT_FALSE(state.due(AWAKE_MS, 1, true, true));             // starts the quiet interval
  EXPECT_FALSE(state.due(AWAKE_MS + QUIET_MS, 2, true, true));  // a vote changed: starts again
  EXPECT_TRUE(state.due(AWAKE_MS + 2 * QUIET_MS, 2, true, true));
}

TEST(XR806StandbyCoordinatorState, RepeatedPmRefusalCountsAsOneFailure) {
  XR806StandbyCoordinatorState state;
  state.configure(QUIET_MS, AWAKE_MS);
  state.boot(0);
  for (uint8_t i = 1; i < XR806StandbyCoordinatorState::MAX_PM_REFUSALS; ++i)
    EXPECT_FALSE(state.pm_refused(0));
  EXPECT_EQ(state.failures(), 0);
  EXPECT_TRUE(state.pm_refused(0));
  EXPECT_EQ(state.failures(), 1);
  // The count starts again after a failure, and an expected wake clears it
  EXPECT_FALSE(state.pm_refused(0));
  state.finished(0, true);
  for (uint8_t i = 1; i < XR806StandbyCoordinatorState::MAX_PM_REFUSALS; ++i)
    EXPECT_FALSE(state.pm_refused(0));
  EXPECT_EQ(state.failures(), 0);
}

struct ExpectedWakeCase {
  int pm_return;
  uint32_t events;
  int wake_io;
  bool wlan_wake;
  bool expected;
};

TEST(XR806StandbyCoordinatorState, ExpectedWake) {
  const ExpectedWakeCase cases[] = {
      {0, XR806StandbyCoordinatorState::WAKE_TIMER, -1, false, true},
      {0, 1u << 10, 10, false, true},
      {0, 1u << 10, -1, false, false},
      {0, XR806StandbyCoordinatorState::WAKE_WLAN, -1, false, false},
      {0, XR806StandbyCoordinatorState::WAKE_WLAN, -1, true, true},
      {0, XR806StandbyCoordinatorState::WAKE_TIMER | XR806StandbyCoordinatorState::WAKE_SOFTWARE, -1, false, false},
      {-1, XR806StandbyCoordinatorState::WAKE_TIMER, -1, false, false},
      {-1, XR806StandbyCoordinatorState::WAKE_WLAN, -1, true, true},
  };
  for (const auto &c : cases) {
    EXPECT_EQ(XR806StandbyCoordinatorState::expected_wake(c.pm_return, c.events, c.wake_io, c.wlan_wake), c.expected)
        << "pm_return " << c.pm_return << " events 0x" << std::hex << c.events;
  }
}

}  // namespace esphome::xr806::testing
