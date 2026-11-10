#include <gtest/gtest.h>
#include "esphome/components/uart/uart_idle_guard.h"

namespace esphome::uart::testing {

TEST(UARTIdleGuard, QuietPeriodBeginsOnlyAfterHardwareIdle) {
  UARTIdleGuard guard;
  EXPECT_FALSE(guard.ready(false, 100, 200));
  // A long 9600-baud TX is not allowed to consume the quiet-time budget.
  EXPECT_FALSE(guard.ready(false, 700, 200));
  EXPECT_FALSE(guard.ready(true, 701, 200));
  EXPECT_FALSE(guard.ready(true, 900, 200));
  EXPECT_TRUE(guard.ready(true, 901, 200));
  guard.reset();  // RX or TX activity before a PM handoff reclaims the lock.
  EXPECT_FALSE(guard.ready(true, 950, 200));
  EXPECT_FALSE(guard.ready(true, 1149, 200));
  EXPECT_TRUE(guard.ready(true, 1150, 200));
  EXPECT_FALSE(guard.ready(false, 1151, 200));
  EXPECT_FALSE(guard.ready(true, 1152, 200));
}

TEST(UARTIdleGuard, MillisWrapAndZeroTimestamp) {
  UARTIdleGuard guard;
  EXPECT_FALSE(guard.ready(true, 0xFFFFFFF0u, 200));
  EXPECT_FALSE(guard.ready(true, 0x000000B7u, 200));
  EXPECT_TRUE(guard.ready(true, 0x000000B8u, 200));
  guard.reset();
  EXPECT_FALSE(guard.ready(true, 0, 200));
  EXPECT_TRUE(guard.ready(true, 200, 200));
}

}  // namespace esphome::uart::testing
