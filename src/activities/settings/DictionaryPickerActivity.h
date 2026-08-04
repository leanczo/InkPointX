#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DictionaryPickerActivity final : public Activity {
 public:
  DictionaryPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DictionaryPicker", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void scan();
  std::vector<std::string> folders_;
  int selectedIndex_ = 0;
  ButtonNavigator navigator_;
};
