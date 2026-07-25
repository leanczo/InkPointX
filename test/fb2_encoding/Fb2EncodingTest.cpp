#include <gtest/gtest.h>

#include "Fb2Encoding.h"

TEST(Fb2Encoding, KeepsAsciiForSupportedEncoding) {
  EXPECT_EQ(Fb2Encoding::decodeByte("windows-1251", 'A'), 'A');
  EXPECT_EQ(Fb2Encoding::decodeByte("KOI8-R", 'z'), 'z');
}

TEST(Fb2Encoding, DecodesWindows1251Russian) {
  EXPECT_EQ(Fb2Encoding::decodeByte("windows-1251", 0xCF), 0x041F);  // П
  EXPECT_EQ(Fb2Encoding::decodeByte("CP1251", 0xF0), 0x0440);        // р
  EXPECT_EQ(Fb2Encoding::decodeByte("WINDOWS_1251", 0xB8), 0x0451); // ё
}

TEST(Fb2Encoding, DecodesKoi8Russian) {
  EXPECT_EQ(Fb2Encoding::decodeByte("koi8-r", 0xF0), 0x041F);  // П
  EXPECT_EQ(Fb2Encoding::decodeByte("KOI8R", 0xD2), 0x0440);   // р
  EXPECT_EQ(Fb2Encoding::decodeByte("koi8-r", 0xA3), 0x0451);  // ё
}

TEST(Fb2Encoding, RejectsUnsupportedHighBytes) {
  EXPECT_EQ(Fb2Encoding::decodeByte("shift-jis", 0xC0), -1);
  EXPECT_EQ(Fb2Encoding::decodeByte("windows-1251", 0x98), -1);
}
