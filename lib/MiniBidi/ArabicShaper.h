#pragma once

#include <cstddef>
#include <cstdint>

namespace ArabicShaper {

// Contextually shape Arabic codepoints before the Unicode bidi visual-order
// pass. Input and output may alias. Returns the number of output codepoints.
// Isolated letters remain at their base Unicode codepoint; joined forms use
// Arabic Presentation Forms supported by the compact FiraGO/Noto subsets.
size_t shape(const uint32_t* input, size_t length, uint32_t* output, size_t capacity);

bool containsArabic(const uint32_t* input, size_t length);

}  // namespace ArabicShaper
