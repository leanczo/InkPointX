#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <cctype>
#include <string>

#include "components/icons/logo160.h"
#include "fontIds.h"

namespace BrandScreen {

constexpr int LOGO_SIZE = 160;
constexpr int LOGO_TOP = 270;
constexpr int WORDMARK_TOP = 462;
// Wide tracking is what makes the small wordmark read as a deliberate mark
// rather than a stray caption.
constexpr int WORDMARK_TRACKING = 5;

inline void draw(const GfxRenderer& renderer) {
  const int pageWidth = renderer.getScreenWidth();
  renderer.drawImage(Logo160Icon, (pageWidth - LOGO_SIZE) / 2, LOGO_TOP, LOGO_SIZE, LOGO_SIZE);

  // Minimal wordmark: uppercase, letterspaced, regular weight at caption size —
  // replaces the former 36 px semibold line under the logo.
  std::string name = I18N.get(StrId::STR_CROSSPOINT);
  for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  int totalWidth = 0;
  for (size_t i = 0; i < name.size(); ++i) {
    const char glyph[2] = {name[i], '\0'};
    totalWidth += renderer.getTextWidth(UI_12_FONT_ID, glyph);
    if (i + 1 < name.size()) totalWidth += WORDMARK_TRACKING;
  }

  int x = (pageWidth - totalWidth) / 2;
  for (size_t i = 0; i < name.size(); ++i) {
    const char glyph[2] = {name[i], '\0'};
    renderer.drawText(UI_12_FONT_ID, x, WORDMARK_TOP, glyph, true);
    x += renderer.getTextWidth(UI_12_FONT_ID, glyph) + WORDMARK_TRACKING;
  }
}

}  // namespace BrandScreen
