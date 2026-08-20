#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// A checklist line's title, plus every following non-checklist line up to
// the next checklist marker (or EOF) -- e.g. a Jira-style task followed by a
// free-text description on the next line(s). A non-checklist line with no
// checklist line above it (or between two checklist lines with no
// description) is its own plain NoteItem with an empty description.
struct NoteItem {
  std::string title;
  bool checklist = false;
  bool done = false;
  std::vector<std::string> description;
};

enum class NotesChecklistView { List, Detail };

// Renders a .txt/.md file from /notes as a navigable checklist. Lines
// starting with "- [ ] " / "- [] " / "- [x] " / "- [X] " (Markdown task-list
// syntax, tolerating editors that can't type a literal space between empty
// brackets) become toggleable rows -- Confirm flips them and rewrites the
// change straight back to the file on disk. Any following non-checklist
// line(s) are grouped as that item's description instead of their own list
// rows, shown as a short preview under the title in the list and in full in
// the Detail view (Right, or Confirm on a non-checklist row).
class NotesChecklistActivity final : public Activity {
 private:
  std::string filePath;
  std::vector<NoteItem> items;
  int selectedIndex = 0;
  // Above kMaxFileBytes the file isn't loaded into this editable view at all
  // (see NotesChecklistActivity.cpp) -- Confirm instead offers to open it in
  // the normal paginated TxtReaderActivity.
  bool tooLarge = false;

  // Card-list scroll state for the List view -- variable-height cards (title
  // + up to 2 lines of description), same growing-window approach as
  // RssActivity's feed cards. Not used by the Detail view, which has its own
  // line-based scroll below.
  int itemsScrollOffset = 0;
  int lastVisibleItemIndex = 0;

  NotesChecklistView view = NotesChecklistView::List;
  int detailScrollOffset = 0;
  int detailMaxLines = 1;

  bool loadFile();
  void saveFile();
  void toggleSelected();
  void openDetail();
  std::string headerTitle() const;

 public:
  NotesChecklistActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath)
      : Activity("NotesChecklist", renderer, mappedInput), filePath(std::move(filePath)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
