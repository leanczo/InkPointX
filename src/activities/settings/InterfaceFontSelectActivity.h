#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class InterfaceFontSelectActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

 public:
  explicit InterfaceFontSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("InterfaceFontSelect", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
