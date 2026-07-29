#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "ArabicShaper.h"

extern "C" {
#include "minibidi.h"
}

namespace {

template <size_t N>
std::array<uint32_t, N> shapeSameLength(const std::array<uint32_t, N>& logical) {
  std::array<uint32_t, N> shaped{};
  EXPECT_EQ(ArabicShaper::shape(logical.data(), logical.size(), shaped.data(), shaped.size()), N);
  return shaped;
}

}  // namespace

TEST(ArabicShaper, SelectsInitialMedialAndFinalForms) {
  // مرحبا
  constexpr std::array<uint32_t, 5> logical = {0x0645, 0x0631, 0x062D, 0x0628, 0x0627};
  constexpr std::array<uint32_t, 5> expected = {0xFEE3, 0xFEAE, 0xFEA3, 0xFE92, 0xFE8E};
  EXPECT_EQ(shapeSameLength(logical), expected);
}

TEST(ArabicShaper, CollapsesLamAlefAndKeepsFollowingLetter) {
  // سلام
  constexpr std::array<uint32_t, 4> logical = {0x0633, 0x0644, 0x0627, 0x0645};
  std::array<uint32_t, 4> shaped{};
  const size_t count = ArabicShaper::shape(logical.data(), logical.size(), shaped.data(), shaped.size());
  ASSERT_EQ(count, 3U);
  EXPECT_EQ(shaped[0], 0xFEB3U);
  EXPECT_EQ(shaped[1], 0xFEFCU);
  EXPECT_EQ(shaped[2], 0x0645U);
}

TEST(ArabicShaper, TransparentMarksDoNotBreakJoining) {
  constexpr std::array<uint32_t, 3> logical = {0x0628, 0x064E, 0x0628};
  constexpr std::array<uint32_t, 3> expected = {0xFE91, 0x064E, 0xFE90};
  EXPECT_EQ(shapeSameLength(logical), expected);
}

TEST(ArabicShaper, ZwnjExplicitlyBreaksJoining) {
  constexpr std::array<uint32_t, 3> logical = {0x0628, 0x200C, 0x0628};
  EXPECT_EQ(shapeSameLength(logical), logical);
}

TEST(ArabicShaper, LeavesMixedLatinAndNumbersUntouched) {
  constexpr std::array<uint32_t, 7> logical = {'A', 'u', 't', 'h', 'o', 'r', '1'};
  EXPECT_FALSE(ArabicShaper::containsArabic(logical.data(), logical.size()));
  EXPECT_EQ(shapeSameLength(logical), logical);
}

TEST(ArabicBidi, ClassifiesArabicLettersDigitsMarksAndForms) {
  EXPECT_EQ(bidi_class(0x0627), AL);
  EXPECT_EQ(bidi_class(0x0661), AN);
  EXPECT_EQ(bidi_class(0x0651), NSM);
  EXPECT_EQ(bidi_class(0xFE8E), AL);
}

TEST(ArabicBidi, ReordersShapedWordToVisualOrder) {
  // Logical contextual forms for مرحبا.
  constexpr std::array<uint32_t, 5> logical = {0xFEE3, 0xFEAE, 0xFEA3, 0xFE92, 0xFE8E};
  constexpr std::array<uint32_t, 5> expectedVisual = {0xFE8E, 0xFE92, 0xFEA3, 0xFEAE, 0xFEE3};
  std::array<bidi_char, 5> line{};
  for (size_t i = 0; i < logical.size(); ++i) {
    line[i].origwc = line[i].wc = logical[i];
    line[i].index = static_cast<uint16_t>(i);
  }
  do_bidi(true, 0, line.data(), static_cast<int>(line.size()));
  for (size_t i = 0; i < line.size(); ++i) {
    EXPECT_EQ(line[i].wc, expectedVisual[i]);
  }
}
