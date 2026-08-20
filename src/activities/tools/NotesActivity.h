#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Lists the flat contents of /notes (created on first entry if missing).
// .txt/.md open in NotesChecklistActivity; .epub delegates straight to the
// normal EPUB reader via onSelectBook(). Deliberately not FileBrowserActivity:
// that dispatches every .txt/.md straight into TxtReaderActivity's passive
// paginated view, with no way to offer the checklist view instead.
class NotesActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;
  std::vector<std::string> files;  // filenames only (no path), sorted, /notes contents
  int selectedIndex = 0;

  void loadFiles();
  void openSelected();

 public:
  explicit NotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Notes", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
