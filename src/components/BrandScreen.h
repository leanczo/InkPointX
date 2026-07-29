#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "components/icons/logo160.h"
#include "fontIds.h"

namespace BrandScreen {

constexpr int LOGO_SIZE = 160;
constexpr int LOGO_TOP = 270;
constexpr int WORDMARK_TOP = 454;

inline void draw(const GfxRenderer& renderer) {
  const int pageWidth = renderer.getScreenWidth();
  renderer.drawImage(Logo160Icon, (pageWidth - LOGO_SIZE) / 2, LOGO_TOP, LOGO_SIZE, LOGO_SIZE);
  renderer.drawCenteredText(WORDMARK_FONT_ID, WORDMARK_TOP, I18N.get(StrId::STR_CROSSPOINT), true,
                            EpdFontFamily::BOLD);
}

}  // namespace BrandScreen
