#pragma once

#include <I18nKeys.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Picks the face the interface is drawn in, or the handwritten accent face.
// Both lists have the same shape — the built-in face first, then every family
// found on the card — so one screen serves both, and a font installed on the
// card is offered everywhere a font is chosen rather than only to the reader.
class InterfaceFontSelectActivity final : public Activity {
 public:
  enum class Target : uint8_t { Interface, Accent };

  explicit InterfaceFontSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       Target target = Target::Interface)
      : Activity("InterfaceFontSelect", renderer, mappedInput), target(target) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  Target target;
  int selectedIndex = 0;
  // Index 0 is the built-in face and carries an empty name; the rest are card
  // family names in the registry's alphabetical order.
  std::vector<std::string> options;

  void rebuildOptions();
  char* targetSetting() const;
  StrId builtInNameId() const;
};
