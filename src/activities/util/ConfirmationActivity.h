#pragma once
#include <functional>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "fontIds.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;

  const int margin = 24;
  const int spacing = 14;
  const int headingFontId = HEADER_FONT_ID;
  const int bodyFontId = UI_10_FONT_ID;

  std::vector<std::string> safeHeadingLines;
  std::vector<std::string> safeBodyLines;
  int cardX = 0;
  int cardY = 0;
  int cardWidth = 0;
  int cardHeight = 0;
  int headingLineHeight = 0;
  int bodyLineHeight = 0;

  // A confirmation is normally opened by a Confirm press. Its physical
  // release can arrive after this activity has already become current; do not
  // mistake that trailing edge for a second, affirmative click.
  bool inputArmed = false;
  bool confirmPressSeen = false;
  bool cancelPressSeen = false;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};
