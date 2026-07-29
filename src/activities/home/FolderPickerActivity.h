#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FolderPickerActivity final : public Activity {
 public:
  FolderPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/")
      : Activity("FolderPicker", renderer, mappedInput), basepath(std::move(initialPath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr size_t NAME_BUFFER_SIZE = 384;

  std::string basepath;
  std::vector<std::string> directories;
  std::unique_ptr<char[]> fileNameBuffer;
  size_t selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  void loadDirectories();
  void goUp();
};
