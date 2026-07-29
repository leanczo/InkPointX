#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class GalleryActivity final : public Activity {
 public:
  GalleryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Gallery", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr size_t MAX_IMAGES = 256;
  static constexpr size_t NAME_BUFFER_SIZE = 384;

  std::vector<std::string> images;
  std::unique_ptr<char[]> fileNameBuffer;
  size_t selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  void scanImages();
  static std::string displayName(const std::string& path);
  static std::string displayFolder(const std::string& path);
};
