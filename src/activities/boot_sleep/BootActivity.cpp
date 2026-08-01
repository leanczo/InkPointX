#include "BootActivity.h"

#include <GfxRenderer.h>

#include "components/BrandScreen.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  renderer.clearScreen();
  BrandScreen::draw(renderer);
  // The boot/unlock presentation is intentionally brand-only.
  renderer.markFrameOverlayDrawn();
  renderer.displayBuffer();
}
