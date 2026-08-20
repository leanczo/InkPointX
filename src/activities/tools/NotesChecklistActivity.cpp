#include "NotesChecklistActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/lucide_ui.h"
#include "fontIds.h"

namespace {
// Conservative cap for loading a note fully into memory to edit -- well
// under HalStorage::readFile's own 50000-byte cap, so a huge stray .txt
// dropped into /notes never gets silently truncated by a save-back. Above
// this, fall back to the normal paginated TxtReaderActivity (read-only here,
// but safe for files of any size).
constexpr size_t kMaxFileBytes = 16384;

struct ChecklistPrefix {
  const char* text;
  size_t len;
  bool done;
};
// "- [] " (no interior space) is included alongside the standard "- [ ] "
// because some external note-taking apps that feed /notes can't type a
// literal space between empty brackets -- see field report. Saving a file
// always normalizes back to "- [ ] "/"- [x] ", so this only matters on read.
constexpr ChecklistPrefix kChecklistPrefixes[] = {
    {"- [ ] ", 6, false},
    {"- [] ", 5, false},
    {"- [x] ", 6, true},
    {"- [X] ", 6, true},
};

bool matchChecklistPrefix(const std::string& raw, bool& outDone, size_t& outLen) {
  for (const auto& prefix : kChecklistPrefixes) {
    if (raw.rfind(prefix.text, 0) == 0) {
      outDone = prefix.done;
      outLen = prefix.len;
      return true;
    }
  }
  return false;
}

// Groups raw lines into NoteItems: a checklist line starts a new item and
// absorbs every following non-checklist line as its description, up to the
// next checklist line or EOF. A non-checklist line with no checklist line
// above it (or between two checklist lines with nothing to absorb) becomes
// its own plain, non-toggleable item.
std::vector<NoteItem> groupLines(const std::vector<std::string>& rawLines) {
  std::vector<NoteItem> items;
  items.reserve(rawLines.size());

  size_t i = 0;
  while (i < rawLines.size()) {
    bool done = false;
    size_t prefixLen = 0;
    if (matchChecklistPrefix(rawLines[i], done, prefixLen)) {
      NoteItem item;
      item.checklist = true;
      item.done = done;
      item.title = rawLines[i].substr(prefixLen);
      i++;
      while (i < rawLines.size() && !matchChecklistPrefix(rawLines[i], done, prefixLen)) {
        item.description.push_back(rawLines[i]);
        i++;
      }
      items.push_back(std::move(item));
    } else {
      NoteItem item;
      item.title = rawLines[i];
      items.push_back(std::move(item));
      i++;
    }
  }
  return items;
}

std::string serializeItem(const NoteItem& item) {
  std::string out;
  if (item.checklist) {
    out += item.done ? "- [x] " : "- [ ] ";
    out += item.title;
  } else {
    out += item.title;
  }
  for (const auto& descLine : item.description) {
    out += "\n";
    out += descLine;
  }
  return out;
}

// One wrapped visual line of the Detail view -- bold for the item's title,
// regular for its description. Distinct from RssActivity's own DetailLine
// (anonymous-namespace local to that file): same shape, different purpose.
struct DetailLine {
  std::string text;
  bool bold;
};
}  // namespace

bool NotesChecklistActivity::loadFile() {
  items.clear();
  tooLarge = false;

  HalFile f;
  if (!Storage.openFileForRead("Notes", filePath, f)) return false;

  const size_t size = f.fileSize();
  if (size > kMaxFileBytes) {
    f.close();
    tooLarge = true;
    return false;
  }

  std::string content;
  content.resize(size);
  if (size > 0) {
    const int readBytes = f.read(content.data(), size);
    content.resize(readBytes > 0 ? static_cast<size_t>(readBytes) : 0);
  }
  f.close();

  std::vector<std::string> rawLines;
  size_t start = 0;
  while (start < content.size()) {
    const size_t nl = content.find('\n', start);
    if (nl == std::string::npos) {
      rawLines.push_back(content.substr(start));
      break;
    }
    std::string raw = content.substr(start, nl - start);
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    rawLines.push_back(std::move(raw));
    start = nl + 1;
  }

  items = groupLines(rawLines);
  return true;
}

void NotesChecklistActivity::saveFile() {
  std::string content;
  content.reserve(items.size() * 32);
  for (const auto& item : items) {
    content += serializeItem(item);
    content += "\n";
  }

  const std::string tmpPath = filePath + ".tmp";
  HalFile f;
  if (!Storage.openFileForWrite("Notes", tmpPath, f)) {
    LOG_ERR("Notes", "Failed to open temp file for save");
    return;
  }
  f.write(content.data(), content.size());
  f.flush();
  f.close();

  if (!Storage.replaceFileFromTemp(filePath.c_str(), tmpPath.c_str())) {
    LOG_ERR("Notes", "Failed to save checklist changes");
  }
}

void NotesChecklistActivity::toggleSelected() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(items.size())) return;
  NoteItem& item = items[static_cast<size_t>(selectedIndex)];
  if (!item.checklist) return;
  item.done = !item.done;
  saveFile();
}

void NotesChecklistActivity::openDetail() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(items.size())) return;
  view = NotesChecklistView::Detail;
  detailScrollOffset = 0;
  requestUpdate();
}

std::string NotesChecklistActivity::headerTitle() const {
  const size_t slash = filePath.find_last_of('/');
  std::string name = (slash == std::string::npos) ? filePath : filePath.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
  return name;
}

void NotesChecklistActivity::onEnter() {
  Activity::onEnter();
  loadFile();
  selectedIndex = 0;
  itemsScrollOffset = 0;
  view = NotesChecklistView::List;
  detailScrollOffset = 0;
  requestUpdate();
}

void NotesChecklistActivity::loop() {
  using Button = MappedInputManager::Button;

  if (tooLarge) {
    if (mappedInput.wasReleased(Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasReleased(Button::Confirm)) {
      onSelectBook(filePath);  // fall back to the normal paginated text reader
    }
    return;
  }

  if (view == NotesChecklistView::Detail) {
    if (mappedInput.wasReleased(Button::Back)) {
      view = NotesChecklistView::List;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(Button::Up)) {
      if (detailScrollOffset > 0) {
        detailScrollOffset = std::max(0, detailScrollOffset - detailMaxLines);
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(Button::Down)) {
      // render() re-clamps this to the last valid page, so it's safe to
      // overshoot here -- a full-page jump so the screen fully replaces
      // instead of shifting by a single row each press.
      detailScrollOffset += detailMaxLines;
      requestUpdate();
    } else if (mappedInput.wasPressed(Button::Confirm)) {
      toggleSelected();
      requestUpdate();
    }
    return;
  }

  // List view.
  if (mappedInput.wasReleased(Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(Button::Right)) {
    openDetail();
    return;
  }

  if (mappedInput.wasPressed(Button::Confirm)) {
    const bool selectedIsChecklist = selectedIndex >= 0 && selectedIndex < static_cast<int>(items.size()) &&
                                     items[static_cast<size_t>(selectedIndex)].checklist;
    if (selectedIsChecklist) {
      toggleSelected();
      requestUpdate();
    } else {
      openDetail();
    }
    return;
  }

  // Single-step Up/Down with a growing scroll window -- same approach as
  // RssActivity's feed cards, needed because these are variable-height cards
  // (title + up to 2 lines of description), not a fixed-row GUI.drawList.
  if (mappedInput.wasReleased(Button::Up)) {
    if (selectedIndex > 0) {
      selectedIndex--;
      if (selectedIndex < itemsScrollOffset) itemsScrollOffset = selectedIndex;
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(Button::Down)) {
    if (selectedIndex < static_cast<int>(items.size()) - 1) {
      if (selectedIndex >= lastVisibleItemIndex) {
        // Already at the bottom of the current page -- jump the whole window
        // to the next unseen item instead of scrolling by one card, so the
        // page fully replaces and never re-shows an already-read item.
        itemsScrollOffset = selectedIndex + 1;
        selectedIndex = itemsScrollOffset;
      } else {
        selectedIndex++;
      }
      requestUpdate();
    }
  }
}

void NotesChecklistActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int headerMaxWidth = pageWidth - 2 * metrics.contentSidePadding;
  const std::string truncatedTitle = renderer.truncatedText(SCRIPT_FONT_ID, headerTitle().c_str(), headerMaxWidth);
  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, truncatedTitle.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  if (tooLarge) {
    renderer.drawCenteredText(UI_12_FONT_ID, contentTop + contentHeight / 2, tr(STR_NOTES_TOO_LARGE), true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NOTES_OPEN_AS_TEXT), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const bool hasValidSelection = selectedIndex >= 0 && selectedIndex < static_cast<int>(items.size());

  if (view == NotesChecklistView::Detail && hasValidSelection) {
    const auto& item = items[static_cast<size_t>(selectedIndex)];
    const int fontId = UI_12_FONT_ID;
    const int lineHeight = renderer.getLineHeight(fontId);
    const int wrapWidth = pageWidth - 2 * metrics.contentSidePadding;

    const std::string fullTitle = (item.checklist ? (item.done ? "[x] " : "[ ] ") : "") + item.title;
    auto titleLines = renderer.wrappedText(fontId, fullTitle.c_str(), wrapWidth, 200, EpdFontFamily::BOLD);

    std::vector<DetailLine> lines;
    lines.reserve(titleLines.size() + 1 + item.description.size() * 2);
    for (auto& l : titleLines) lines.push_back({std::move(l), true});
    if (!item.description.empty()) lines.push_back({"", false});
    for (const auto& descLine : item.description) {
      if (descLine.empty()) {
        lines.push_back({"", false});
        continue;
      }
      auto wrapped = renderer.wrappedText(fontId, descLine.c_str(), wrapWidth, 200, EpdFontFamily::REGULAR);
      for (auto& w : wrapped) lines.push_back({std::move(w), false});
    }

    const int maxLines = std::max(1, contentHeight / lineHeight);
    detailMaxLines = maxLines;
    if (detailScrollOffset > std::max(0, static_cast<int>(lines.size()) - maxLines)) {
      detailScrollOffset = std::max(0, static_cast<int>(lines.size()) - maxLines);
    }

    for (int i = 0; i < maxLines; i++) {
      const int lineIdx = detailScrollOffset + i;
      if (lineIdx >= static_cast<int>(lines.size())) break;
      const auto& line = lines[static_cast<size_t>(lineIdx)];
      renderer.drawText(fontId, metrics.contentSidePadding, contentTop + i * lineHeight, line.text.c_str(), true,
                        line.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), item.checklist ? tr(STR_SELECT) : nullptr,
                                              tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (items.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, contentTop + contentHeight / 2, tr(STR_NOTES_EMPTY_FILE), true);
  } else {
    // Custom variable-height cards instead of GUI.drawList: a checkbox on the
    // left of the title (drawList's rowAccessory is always right-aligned, and
    // its rowSubtitle is a single truncated line -- neither fits "checkbox,
    // title, up to 2 lines of description" as its own card). Same approach as
    // RssActivity's feed cards: a rounded-rect border per card doubles as the
    // separator between items, thicker when selected.
    const int cellX = metrics.contentSidePadding;
    const int cellW = pageWidth - 2 * metrics.contentSidePadding;
    const int fontId = UI_10_FONT_ID;
    const int lineHeight = renderer.getLineHeight(fontId);
    const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    constexpr int kCardPadding = 12;
    constexpr int kCardGap = 10;
    constexpr int kSectionGap = 6;
    constexpr int kMaxTitleLines = 2;
    constexpr int kMaxDescLines = 2;
    constexpr int kCheckboxSize = 24;
    constexpr int kCheckboxGap = 8;

    auto titleIndent = [&](const NoteItem& item) { return item.checklist ? kCheckboxSize + kCheckboxGap : 0; };

    // Up to kMaxDescLines wrapped preview lines, skipping blank separator
    // lines the user's own editor may have left in the description -- those
    // read as intentional formatting in the full Detail view but would just
    // waste a line in this compact preview.
    auto descLinesFor = [&](const NoteItem& item) {
      std::vector<std::string> result;
      for (const auto& raw : item.description) {
        if (static_cast<int>(result.size()) >= kMaxDescLines) break;
        if (raw.empty()) continue;
        auto wrapped = renderer.wrappedText(fontId, raw.c_str(), cellW - 2 * kCardPadding,
                                            kMaxDescLines - static_cast<int>(result.size()), EpdFontFamily::REGULAR);
        for (auto& w : wrapped) {
          if (static_cast<int>(result.size()) >= kMaxDescLines) break;
          result.push_back(std::move(w));
        }
      }
      return result;
    };

    auto cardHeightFor = [&](const NoteItem& item) {
      auto titleLines = renderer.wrappedText(fontId, item.title.c_str(), cellW - 2 * kCardPadding - titleIndent(item),
                                             kMaxTitleLines, EpdFontFamily::BOLD);
      int h = kCardPadding + std::max(1, static_cast<int>(titleLines.size())) * lineHeight;
      auto descLines = descLinesFor(item);
      if (!descLines.empty()) h += kSectionGap + static_cast<int>(descLines.size()) * lineHeight;
      return h + kCardPadding + kCardGap;
    };

    // Grow itemsScrollOffset until the selected card is visible -- scrolling
    // upward is already snapped by loop()'s Up handler; this only grows the
    // window past the bottom, and needs the real (variable) card heights.
    if (selectedIndex >= itemsScrollOffset) {
      while (true) {
        int yy = contentTop;
        int lastVisible = itemsScrollOffset - 1;
        for (int idx = itemsScrollOffset; idx < static_cast<int>(items.size()); idx++) {
          const int h = cardHeightFor(items[static_cast<size_t>(idx)]);
          if (yy + h > contentBottom) break;
          yy += h;
          lastVisible = idx;
        }
        if (selectedIndex <= lastVisible || itemsScrollOffset >= selectedIndex) break;
        itemsScrollOffset++;
      }
    }

    int cellY = contentTop;
    for (int idx = itemsScrollOffset; idx < static_cast<int>(items.size()); idx++) {
      const auto& item = items[static_cast<size_t>(idx)];
      const int cardH = cardHeightFor(item);
      if (cellY + cardH > contentBottom) break;
      lastVisibleItemIndex = idx;

      const bool isSelected = (idx == selectedIndex);
      renderer.drawRoundedRect(cellX, cellY, cellW, cardH - kCardGap, isSelected ? 3 : 1, 8, true);

      int textY = cellY + kCardPadding / 2;
      const int indent = titleIndent(item);

      if (item.checklist) {
        // A couple pixels lower than pure line-box centering -- the glyph ink
        // of UI_10_FONT_ID sits low in its line height, so a mathematically
        // centered checkbox reads as sitting a bit high next to the title text.
        constexpr int kCheckboxVerticalNudge = 3;
        const int checkboxY = textY + (lineHeight - kCheckboxSize) / 2 + kCheckboxVerticalNudge;
        renderer.drawIcon(item.done ? LucideSquareCheck24 : LucideSquare24, cellX + kCardPadding, checkboxY,
                          kCheckboxSize, kCheckboxSize);
      }

      auto titleLines = renderer.wrappedText(fontId, item.title.c_str(), cellW - 2 * kCardPadding - indent,
                                             kMaxTitleLines, EpdFontFamily::BOLD);
      for (const auto& line : titleLines) {
        renderer.drawText(fontId, cellX + kCardPadding + indent, textY, line.c_str(), true, EpdFontFamily::BOLD);
        textY += lineHeight;
      }

      auto descLines = descLinesFor(item);
      if (!descLines.empty()) {
        textY += kSectionGap;
        for (const auto& line : descLines) {
          renderer.drawText(fontId, cellX + kCardPadding, textY, line.c_str(), true, EpdFontFamily::REGULAR);
          textY += lineHeight;
        }
      }

      cellY += cardH;
    }
  }

  const char* rightAction = items.empty() ? nullptr : tr(STR_NOTES_DETAIL);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, rightAction);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
