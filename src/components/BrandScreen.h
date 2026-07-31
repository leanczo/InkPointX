#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "components/icons/logo160.h"
#include "fontIds.h"

namespace BrandScreen {

constexpr int LOGO_SIZE = 160;
constexpr int LOGO_TOP = 270;
constexpr int WORDMARK_TOP = 452;

inline void draw(const GfxRenderer& renderer) {
  const int pageWidth = renderer.getScreenWidth();
  renderer.drawImage(Logo160Icon, (pageWidth - LOGO_SIZE) / 2, LOGO_TOP, LOGO_SIZE, LOGO_SIZE);

  // Wordmark in the handwritten accent face — the same voice as the Home
  // greeting and author line, and warmer than the tracked caps it replaces.
  const char* name = I18N.get(StrId::STR_CROSSPOINT);
  const int width = renderer.getTextWidth(SCRIPT_FONT_ID, name);
  renderer.drawText(SCRIPT_FONT_ID, (pageWidth - width) / 2, WORDMARK_TOP, name);
}

}  // namespace BrandScreen
