#include <gtest/gtest.h>

#include <string>

#include "Fb2Encoding.h"

namespace {
// Byte fixtures generated from the same source sentence in each encoding, so a
// detection change cannot be papered over by editing the expectation. Every byte
// is escaped because an unescaped letter after a hex escape would be absorbed
// into it.
// CP1251: Привет мир, это обычный русский текст для проверки определения кодировки.
constexpr char kWindows1251Russian[] = 
    "\xcf\xf0\xe8\xe2\xe5\xf2\x20\xec\xe8\xf0\x2c\x20\xfd\xf2\xee\x20\xee\xe1"
    "\xfb\xf7\xed\xfb\xe9\x20\xf0\xf3\xf1\xf1\xea\xe8\xe9\x20\xf2\xe5\xea\xf1"
    "\xf2\x20\xe4\xeb\xff\x20\xef\xf0\xee\xe2\xe5\xf0\xea\xe8\x20\xee\xef\xf0"
    "\xe5\xe4\xe5\xeb\xe5\xed\xe8\xff\x20\xea\xee\xe4\xe8\xf0\xee\xe2\xea\xe8"
    "\x2e";
// KOI8-R: Привет мир, это обычный русский текст для проверки определения кодировки.
constexpr char kKoi8Russian[] = 
    "\xf0\xd2\xc9\xd7\xc5\xd4\x20\xcd\xc9\xd2\x2c\x20\xdc\xd4\xcf\x20\xcf\xc2"
    "\xd9\xde\xce\xd9\xca\x20\xd2\xd5\xd3\xd3\xcb\xc9\xca\x20\xd4\xc5\xcb\xd3"
    "\xd4\x20\xc4\xcc\xd1\x20\xd0\xd2\xcf\xd7\xc5\xd2\xcb\xc9\x20\xcf\xd0\xd2"
    "\xc5\xc4\xc5\xcc\xc5\xce\xc9\xd1\x20\xcb\xcf\xc4\xc9\xd2\xcf\xd7\xcb\xc9"
    "\x2e";
// UTF-8: Привет мир, это обычный русский текст для проверки определения кодировки.
constexpr char kUtf8Russian[] = 
    "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\x20\xd0\xbc\xd0\xb8\xd1"
    "\x80\x2c\x20\xd1\x8d\xd1\x82\xd0\xbe\x20\xd0\xbe\xd0\xb1\xd1\x8b\xd1\x87"
    "\xd0\xbd\xd1\x8b\xd0\xb9\x20\xd1\x80\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0"
    "\xb8\xd0\xb9\x20\xd1\x82\xd0\xb5\xd0\xba\xd1\x81\xd1\x82\x20\xd0\xb4\xd0"
    "\xbb\xd1\x8f\x20\xd0\xbf\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb5\xd1\x80\xd0\xba"
    "\xd0\xb8\x20\xd0\xbe\xd0\xbf\xd1\x80\xd0\xb5\xd0\xb4\xd0\xb5\xd0\xbb\xd0"
    "\xb5\xd0\xbd\xd0\xb8\xd1\x8f\x20\xd0\xba\xd0\xbe\xd0\xb4\xd0\xb8\xd1\x80"
    "\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb8\x2e";
// UTF-8 with BOM: Привет мир, это обычный русский текст для проверки определения кодировки.
constexpr char kUtf8RussianWithBom[] = 
    "\xef\xbb\xbf\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\x20\xd0\xbc"
    "\xd0\xb8\xd1\x80\x2c\x20\xd1\x8d\xd1\x82\xd0\xbe\x20\xd0\xbe\xd0\xb1\xd1"
    "\x8b\xd1\x87\xd0\xbd\xd1\x8b\xd0\xb9\x20\xd1\x80\xd1\x83\xd1\x81\xd1\x81"
    "\xd0\xba\xd0\xb8\xd0\xb9\x20\xd1\x82\xd0\xb5\xd0\xba\xd1\x81\xd1\x82\x20"
    "\xd0\xb4\xd0\xbb\xd1\x8f\x20\xd0\xbf\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb5\xd1"
    "\x80\xd0\xba\xd0\xb8\x20\xd0\xbe\xd0\xbf\xd1\x80\xd0\xb5\xd0\xb4\xd0\xb5"
    "\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f\x20\xd0\xba\xd0\xbe\xd0\xb4\xd0"
    "\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb8\x2e";
// ASCII: The quick brown fox jumps over the lazy dog, plainly and repeatedly.
constexpr char kAsciiEnglish[] = 
    "\x54\x68\x65\x20\x71\x75\x69\x63\x6b\x20\x62\x72\x6f\x77\x6e\x20\x66\x6f"
    "\x78\x20\x6a\x75\x6d\x70\x73\x20\x6f\x76\x65\x72\x20\x74\x68\x65\x20\x6c"
    "\x61\x7a\x79\x20\x64\x6f\x67\x2c\x20\x70\x6c\x61\x69\x6e\x6c\x79\x20\x61"
    "\x6e\x64\x20\x72\x65\x70\x65\x61\x74\x65\x64\x6c\x79\x2e";
// CP1252: Voilà une phrase française avec des accents: éàçùè, répétée plusieurs fois.
constexpr char kWindows1252French[] = 
    "\x56\x6f\x69\x6c\xe0\x20\x75\x6e\x65\x20\x70\x68\x72\x61\x73\x65\x20\x66"
    "\x72\x61\x6e\xe7\x61\x69\x73\x65\x20\x61\x76\x65\x63\x20\x64\x65\x73\x20"
    "\x61\x63\x63\x65\x6e\x74\x73\x3a\x20\xe9\xe0\xe7\xf9\xe8\x2c\x20\x72\xe9"
    "\x70\xe9\x74\xe9\x65\x20\x70\x6c\x75\x73\x69\x65\x75\x72\x73\x20\x66\x6f"
    "\x69\x73\x2e";

// Helper: detect() takes an explicit length because a sample may contain
// high bytes that are not valid C string terminators.
template <size_t N>
const char* detectLiteral(const char (&literal)[N]) {
  return Fb2Encoding::detect(literal, N - 1);
}
}  // namespace

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

TEST(Fb2Encoding, DecodesAddedWesternAndCyrillicTables) {
  EXPECT_EQ(Fb2Encoding::decodeByte("windows-1252", 0xE9), 0x00E9);  // é
  EXPECT_EQ(Fb2Encoding::decodeByte("iso-8859-1", 0xFC), 0x00FC);    // ü
  EXPECT_EQ(Fb2Encoding::decodeByte("iso-8859-5", 0xE0), 0x0440);    // р
  EXPECT_EQ(Fb2Encoding::decodeByte("cp866", 0x80), 0x0410);         // А
}

TEST(Fb2Encoding, ReportsWhichEncodingsAreSingleByte) {
  EXPECT_TRUE(Fb2Encoding::isSupported("windows-1251"));
  EXPECT_TRUE(Fb2Encoding::isSupported("koi8-r"));
  EXPECT_TRUE(Fb2Encoding::isSupported("windows-1252"));
  // UTF-8 is multi-byte and shift-jis is unsupported: both must transcode as a
  // pass-through so callers never mangle them.
  EXPECT_FALSE(Fb2Encoding::isSupported(Fb2Encoding::UTF8));
  EXPECT_FALSE(Fb2Encoding::isSupported("shift-jis"));
}

TEST(Fb2Encoding, DetectsSingleByteCyrillicApart) {
  // CP1251 and KOI8-R put Cyrillic lowercase in opposite halves of the high
  // range, which is what separates them on a run of ordinary prose.
  EXPECT_STREQ(detectLiteral(kWindows1251Russian), Fb2Encoding::WINDOWS_1251);
  EXPECT_STREQ(detectLiteral(kKoi8Russian), Fb2Encoding::KOI8_R);
}

TEST(Fb2Encoding, DetectsUtf8AndAscii) {
  EXPECT_STREQ(detectLiteral(kUtf8Russian), Fb2Encoding::UTF8);
  EXPECT_STREQ(detectLiteral(kUtf8RussianWithBom), Fb2Encoding::UTF8);
  EXPECT_STREQ(detectLiteral(kAsciiEnglish), Fb2Encoding::UTF8);
}

TEST(Fb2Encoding, DetectsWesternTextAsWindows1252) {
  EXPECT_STREQ(detectLiteral(kWindows1252French), Fb2Encoding::WINDOWS_1252);
}

TEST(Fb2Encoding, ReportsBomLengthOnlyWhenPresent) {
  EXPECT_EQ(Fb2Encoding::bomLength(kUtf8RussianWithBom, sizeof(kUtf8RussianWithBom) - 1), 3u);
  EXPECT_EQ(Fb2Encoding::bomLength(kUtf8Russian, sizeof(kUtf8Russian) - 1), 0u);
  EXPECT_EQ(Fb2Encoding::bomLength(nullptr, 0), 0u);
}

TEST(Fb2Encoding, TranscodesSingleByteTextToUtf8) {
  const std::string converted =
      Fb2Encoding::toUtf8(Fb2Encoding::WINDOWS_1251, kWindows1251Russian, sizeof(kWindows1251Russian) - 1);
  EXPECT_EQ(converted, std::string(kUtf8Russian, sizeof(kUtf8Russian) - 1));
}

TEST(Fb2Encoding, TranscodeLeavesUnsupportedEncodingsUntouched) {
  const std::string source(kUtf8Russian, sizeof(kUtf8Russian) - 1);
  EXPECT_EQ(Fb2Encoding::toUtf8(Fb2Encoding::UTF8, source.data(), source.size()), source);
  EXPECT_EQ(Fb2Encoding::toUtf8("shift-jis", source.data(), source.size()), source);
}

TEST(Fb2Encoding, TranscodeEmitsOneCodePointPerSourceByte) {
  // The plain-text reader relies on this to map a position in the converted text
  // back to a source-byte offset for its page index.
  const std::string converted =
      Fb2Encoding::toUtf8(Fb2Encoding::WINDOWS_1251, kWindows1251Russian, sizeof(kWindows1251Russian) - 1);
  size_t codePoints = 0;
  for (const char c : converted) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) codePoints++;
  }
  EXPECT_EQ(codePoints, sizeof(kWindows1251Russian) - 1);
}

TEST(Fb2Encoding, AppendsUtf8ForEachCodePointWidth) {
  std::string out;
  Fb2Encoding::appendUtf8(out, 'A');
  Fb2Encoding::appendUtf8(out, 0x0440);  // р, two bytes
  Fb2Encoding::appendUtf8(out, 0x2116);  // №, three bytes
  EXPECT_EQ(out, "A\xd1\x80\xe2\x84\x96");

  // An undefined byte in a legacy table arrives here as -1 and must render as the
  // replacement character rather than corrupting the stream.
  std::string fallback;
  Fb2Encoding::appendUtf8(fallback, -1);
  EXPECT_EQ(fallback, "\xef\xbf\xbd");
}
