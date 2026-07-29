#pragma once

#include <I18n.h>

#include <array>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FileActionsActivity final : public Activity {
 public:
  enum class Action { Open, NewFolder, Copy, Move, Rename, Delete, Properties };

  FileActionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string itemName)
      : Activity("FileActions", renderer, mappedInput), itemName(std::move(itemName)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr std::array<StrId, 7> LABELS = {
      StrId::STR_OPEN,   StrId::STR_NEW_FOLDER, StrId::STR_COPY_TO,    StrId::STR_MOVE_TO,
      StrId::STR_RENAME, StrId::STR_DELETE,     StrId::STR_PROPERTIES,
  };

  std::string itemName;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
};
