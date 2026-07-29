#include <gtest/gtest.h>

#include "hal/EInkRefreshPolicy.h"

TEST(EInkRefreshPolicy, KeepsInteractiveFastRunFlashFreeByDefault) {
  EInkRefreshPolicy policy;

  for (int i = 0; i < 64; ++i) {
    EXPECT_EQ(policy.consume(EInkRefreshPolicy::Mode::Fast), EInkRefreshPolicy::Mode::Fast);
  }
}

TEST(EInkRefreshPolicy, InjectsCleanRefreshWhenAutomaticCleanupIsExplicitlyEnabled) {
  EInkRefreshPolicy policy;
  policy.setAutomaticCleanupEnabled(true);

  for (uint8_t i = 0; i < EInkRefreshPolicy::MAX_CONSECUTIVE_FAST_REFRESHES; ++i) {
    EXPECT_EQ(policy.consume(EInkRefreshPolicy::Mode::Fast), EInkRefreshPolicy::Mode::Fast);
  }
  EXPECT_EQ(policy.consume(EInkRefreshPolicy::Mode::Fast), EInkRefreshPolicy::Mode::Clean);
  EXPECT_EQ(policy.consecutiveFastRefreshes(), 0);
}

TEST(EInkRefreshPolicy, ExplicitCleanPromotesOnlyNextFastFrame) {
  EInkRefreshPolicy policy;
  policy.requestClean();

  EXPECT_EQ(policy.consume(EInkRefreshPolicy::Mode::Fast), EInkRefreshPolicy::Mode::Clean);
  EXPECT_EQ(policy.consume(EInkRefreshPolicy::Mode::Fast), EInkRefreshPolicy::Mode::Fast);
}

TEST(EInkRefreshPolicy, FullRequestHasPriorityAndResetsCadence) {
  EInkRefreshPolicy policy;
  policy.requestClean();
  policy.requestFull();

  EXPECT_EQ(policy.consume(EInkRefreshPolicy::Mode::Fast), EInkRefreshPolicy::Mode::Full);
  EXPECT_EQ(policy.consecutiveFastRefreshes(), 0);
  EXPECT_EQ(policy.consume(EInkRefreshPolicy::Mode::Fast), EInkRefreshPolicy::Mode::Fast);
}

TEST(EInkRefreshPolicy, ReaderCanDisableAutomaticCleanupWithoutBlockingExplicitClean) {
  EInkRefreshPolicy policy;
  policy.setAutomaticCleanupEnabled(false);

  for (int i = 0; i < 32; ++i) {
    EXPECT_EQ(policy.consume(EInkRefreshPolicy::Mode::Fast), EInkRefreshPolicy::Mode::Fast);
  }

  policy.requestClean();
  EXPECT_EQ(policy.consume(EInkRefreshPolicy::Mode::Fast), EInkRefreshPolicy::Mode::Clean);
}
