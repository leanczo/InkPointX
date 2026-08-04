#include "ArabicShaper.h"

#include <cstddef>
#include <cstdint>

namespace {

struct ShapeEntry {
  uint32_t base;
  uint32_t finalForm;
  uint32_t initialForm;
  uint32_t medialForm;
  bool joinsPrevious;
  bool joinsNext;
};

// Isolated forms deliberately use the logical base codepoint. FiraGO and Noto
// Naskh expose isolated outlines there, while their cmap only exposes joined
// presentation forms for most letters.
constexpr ShapeEntry SHAPES[] = {
    {0x0621, 0, 0, 0, false, false},
    {0x0622, 0xFE82, 0, 0, true, false},
    {0x0623, 0xFE84, 0, 0, true, false},
    {0x0624, 0xFE86, 0, 0, true, false},
    {0x0625, 0xFE88, 0, 0, true, false},
    {0x0626, 0xFE8A, 0xFE8B, 0xFE8C, true, true},
    {0x0627, 0xFE8E, 0, 0, true, false},
    {0x0628, 0xFE90, 0xFE91, 0xFE92, true, true},
    {0x0629, 0xFE94, 0, 0, true, false},
    {0x062A, 0xFE96, 0xFE97, 0xFE98, true, true},
    {0x062B, 0xFE9A, 0xFE9B, 0xFE9C, true, true},
    {0x062C, 0xFE9E, 0xFE9F, 0xFEA0, true, true},
    {0x062D, 0xFEA2, 0xFEA3, 0xFEA4, true, true},
    {0x062E, 0xFEA6, 0xFEA7, 0xFEA8, true, true},
    {0x062F, 0xFEAA, 0, 0, true, false},
    {0x0630, 0xFEAC, 0, 0, true, false},
    {0x0631, 0xFEAE, 0, 0, true, false},
    {0x0632, 0xFEB0, 0, 0, true, false},
    {0x0633, 0xFEB2, 0xFEB3, 0xFEB4, true, true},
    {0x0634, 0xFEB6, 0xFEB7, 0xFEB8, true, true},
    {0x0635, 0xFEBA, 0xFEBB, 0xFEBC, true, true},
    {0x0636, 0xFEBE, 0xFEBF, 0xFEC0, true, true},
    {0x0637, 0xFEC2, 0xFEC3, 0xFEC4, true, true},
    {0x0638, 0xFEC6, 0xFEC7, 0xFEC8, true, true},
    {0x0639, 0xFECA, 0xFECB, 0xFECC, true, true},
    {0x063A, 0xFECE, 0xFECF, 0xFED0, true, true},
    {0x0641, 0xFED2, 0xFED3, 0xFED4, true, true},
    {0x0642, 0xFED6, 0xFED7, 0xFED8, true, true},
    {0x0643, 0xFEDA, 0xFEDB, 0xFEDC, true, true},
    {0x0644, 0xFEDE, 0xFEDF, 0xFEE0, true, true},
    {0x0645, 0xFEE2, 0xFEE3, 0xFEE4, true, true},
    {0x0646, 0xFEE6, 0xFEE7, 0xFEE8, true, true},
    {0x0647, 0xFEEA, 0xFEEB, 0xFEEC, true, true},
    {0x0648, 0xFEEE, 0, 0, true, false},
    {0x0649, 0xFEF0, 0, 0, true, false},
    {0x064A, 0xFEF2, 0xFEF3, 0xFEF4, true, true},

    // Common Persian/Urdu letters used in dynamic author/title metadata.
    {0x0671, 0xFB51, 0, 0, true, false},
    {0x067E, 0xFB57, 0xFB58, 0xFB59, true, true},
    {0x0686, 0xFB7B, 0xFB7C, 0xFB7D, true, true},
    {0x0698, 0xFB8B, 0, 0, true, false},
    {0x06A9, 0xFB8F, 0xFB90, 0xFB91, true, true},
    {0x06AF, 0xFB93, 0xFB94, 0xFB95, true, true},
    {0x06CC, 0xFBFD, 0xFBFE, 0xFBFF, true, true},
};

constexpr ShapeEntry TATWEEL = {0x0640, 0, 0, 0, true, true};

const ShapeEntry* findShape(const uint32_t codepoint) {
  if (codepoint == TATWEEL.base) return &TATWEEL;
  for (const auto& entry : SHAPES) {
    if (entry.base == codepoint) return &entry;
  }
  return nullptr;
}

bool isTransparent(const uint32_t codepoint) {
  return (codepoint >= 0x0610 && codepoint <= 0x061A) || (codepoint >= 0x064B && codepoint <= 0x065F) ||
         codepoint == 0x0670 || (codepoint >= 0x06D6 && codepoint <= 0x06ED) || codepoint == 0x200D;
}

int previousJoinCandidate(const uint32_t* input, int index) {
  for (int i = index - 1; i >= 0; --i) {
    if (input[i] == 0x200C) return -1;  // ZWNJ
    if (isTransparent(input[i])) continue;
    return i;
  }
  return -1;
}

int nextJoinCandidate(const uint32_t* input, const size_t length, const size_t index) {
  for (size_t i = index + 1; i < length; ++i) {
    if (input[i] == 0x200C) return -1;  // ZWNJ
    if (isTransparent(input[i])) continue;
    return static_cast<int>(i);
  }
  return -1;
}

bool joinsPrevious(const uint32_t* input, const size_t index) {
  const auto* current = findShape(input[index]);
  if (!current || !current->joinsPrevious) return false;
  const int previousIndex = previousJoinCandidate(input, static_cast<int>(index));
  if (previousIndex < 0) return false;
  const auto* previous = findShape(input[previousIndex]);
  return previous && previous->joinsNext;
}

bool joinsNext(const uint32_t* input, const size_t length, const size_t index) {
  const auto* current = findShape(input[index]);
  if (!current || !current->joinsNext) return false;
  const int nextIndex = nextJoinCandidate(input, length, index);
  if (nextIndex < 0) return false;
  const auto* next = findShape(input[nextIndex]);
  return next && next->joinsPrevious;
}

uint32_t lamAlefForm(const uint32_t alef, const bool connectedBefore) {
  switch (alef) {
    case 0x0622:
      return connectedBefore ? 0xFEF6 : 0xFEF5;
    case 0x0623:
      return connectedBefore ? 0xFEF8 : 0xFEF7;
    case 0x0625:
      return connectedBefore ? 0xFEFA : 0xFEF9;
    case 0x0627:
      return connectedBefore ? 0xFEFC : 0xFEFB;
    default:
      return 0;
  }
}

}  // namespace

namespace ArabicShaper {

bool containsArabic(const uint32_t* input, const size_t length) {
  for (size_t i = 0; i < length; ++i) {
    const uint32_t cp = input[i];
    if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x0870 && cp <= 0x08FF)) {
      return true;
    }
  }
  return false;
}

size_t shape(const uint32_t* input, const size_t length, uint32_t* output, const size_t capacity) {
  if (!input || !output || capacity < length) return 0;

  size_t outputLength = 0;
  for (size_t i = 0; i < length; ++i) {
    const uint32_t codepoint = input[i];
    const auto* entry = findShape(codepoint);
    if (!entry) {
      output[outputLength++] = codepoint;
      continue;
    }

    // Lam-Alef has no meaningful separate visual forms. Collapse it before
    // bidi, retaining whether the Lam connects to the preceding letter.
    if (codepoint == 0x0644 && i + 1 < length) {
      const uint32_t ligature = lamAlefForm(input[i + 1], joinsPrevious(input, i));
      if (ligature) {
        output[outputLength++] = ligature;
        ++i;
        continue;
      }
    }

    const bool connectedBefore = joinsPrevious(input, i);
    const bool connectedAfter = joinsNext(input, length, i);
    uint32_t shaped = codepoint;
    if (connectedBefore && connectedAfter && entry->medialForm) {
      shaped = entry->medialForm;
    } else if (connectedBefore && entry->finalForm) {
      shaped = entry->finalForm;
    } else if (connectedAfter && entry->initialForm) {
      shaped = entry->initialForm;
    }
    output[outputLength++] = shaped;
  }
  return outputLength;
}

}  // namespace ArabicShaper
