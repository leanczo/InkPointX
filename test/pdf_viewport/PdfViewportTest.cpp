#include <gtest/gtest.h>

#include "PdfViewport.h"

TEST(PdfViewport, FitUsesOneCompleteView) {
  const auto view = PdfViewport::calculate(480, 760, 0, 0);
  EXPECT_EQ(view.x, 0);
  EXPECT_EQ(view.y, 0);
  EXPECT_EQ(view.width, 480);
  EXPECT_EQ(view.height, 760);
  EXPECT_EQ(view.count(), 1);
}

TEST(PdfViewport, FractionalZoomOverlapsAndStillReachesEveryEdge) {
  const auto first = PdfViewport::calculate(480, 760, 1, 0);
  const auto last = PdfViewport::calculate(480, 760, 1, 99);

  EXPECT_EQ(first.width, 384);
  EXPECT_EQ(first.height, 608);
  EXPECT_EQ(first.count(), 4);
  EXPECT_EQ(first.x, 0);
  EXPECT_EQ(first.y, 0);
  EXPECT_EQ(last.index, 3);
  EXPECT_EQ(last.x + last.width, 480);
  EXPECT_EQ(last.y + last.height, 760);
  EXPECT_LT(last.x, first.width);
  EXPECT_LT(last.y, first.height);
}

TEST(PdfViewport, TwoHundredPercentProducesFourExactTiles) {
  const auto topRight = PdfViewport::calculate(480, 760, 3, 1);
  const auto bottomLeft = PdfViewport::calculate(480, 760, 3, 2);

  EXPECT_EQ(topRight.width, 240);
  EXPECT_EQ(topRight.height, 380);
  EXPECT_EQ(topRight.x, 240);
  EXPECT_EQ(topRight.y, 0);
  EXPECT_EQ(bottomLeft.x, 0);
  EXPECT_EQ(bottomLeft.y, 380);
}

TEST(PdfViewport, InvalidOptionAndIndexAreClamped) {
  EXPECT_EQ(PdfViewport::zoomPercent(255), 200);
  const auto view = PdfViewport::calculate(528, 752, 255, -12);
  EXPECT_EQ(view.index, 0);
  EXPECT_GT(view.width, 0);
  EXPECT_GT(view.height, 0);
}
