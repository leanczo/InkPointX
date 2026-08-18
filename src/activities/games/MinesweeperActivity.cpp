#include "MinesweeperActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdlib>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void MinesweeperActivity::computeGrid() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const int contentTop = metrics.topPadding + metrics.headerHeight + SUBHEADER_HEIGHT + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  cols = pageWidth / CELL_SIZE;
  rows = contentHeight / CELL_SIZE;
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  if (cols * rows > MAX_CELLS) {
    // Defensive only: CELL_SIZE keeps cols*rows well under MAX_CELLS on this
    // display, but never let a future metrics change overflow the fixed arrays.
    rows = MAX_CELLS / cols;
    if (rows < 1) rows = 1;
  }

  const int gridPixelW = cols * CELL_SIZE;
  const int gridPixelH = rows * CELL_SIZE;
  gridOriginX = (pageWidth - gridPixelW) / 2;
  gridOriginY = contentTop + (contentHeight - gridPixelH) / 2;
}

void MinesweeperActivity::resetGame() {
  computeGrid();
  const int totalCells = cols * rows;
  for (int i = 0; i < totalCells; i++) {
    mine[i] = false;
    revealed[i] = false;
    flagged[i] = false;
    adjacentCount[i] = 0;
  }

  // Guard against a degenerate near-zero-cell grid (should not happen on the
  // real panel, but never let placeMines() loop forever hunting for mine
  // slots that can't exist).
  mineCount = 0;
  if (totalCells >= 2) {
    mineCount = static_cast<int>(totalCells * MINE_DENSITY + 0.5f);
    if (mineCount < 1) mineCount = 1;
    if (mineCount > totalCells - 1) mineCount = totalCells - 1;
  }

  cursorRow = 0;
  cursorCol = cols / 2;
  flagMode = false;
  firstReveal = true;
  gameOver = false;
  won = false;
  flaggedCount = 0;
  revealedSafeCount = 0;
}

void MinesweeperActivity::placeMines(int excludeIndex) {
  const int totalCells = cols * rows;
  int placed = 0;
  while (placed < mineCount) {
    const int idx = rand() % totalCells;
    if (idx == excludeIndex || mine[idx]) continue;
    mine[idx] = true;
    placed++;
  }

  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      const int idx = r * cols + c;
      if (mine[idx]) continue;
      int count = 0;
      for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
          if (dr == 0 && dc == 0) continue;
          const int nr = r + dr;
          const int nc = c + dc;
          if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
          if (mine[nr * cols + nc]) count++;
        }
      }
      adjacentCount[idx] = static_cast<int8_t>(count);
    }
  }
}

void MinesweeperActivity::revealCell(int row, int col) {
  const int startIdx = row * cols + col;
  if (flagged[startIdx] || revealed[startIdx]) return;

  // Iterative flood-fill using the fixed member stack (never a large local)
  // so zero-adjacency cells cascade open like classic Minesweeper without
  // risking a deep call stack on this constrained target.
  int top = 0;
  floodStack[top++] = startIdx;

  while (top > 0) {
    const int idx = floodStack[--top];
    if (revealed[idx] || flagged[idx]) continue;
    revealed[idx] = true;

    if (mine[idx]) {
      gameOver = true;
      return;
    }
    revealedSafeCount++;

    if (adjacentCount[idx] == 0) {
      const int r = idx / cols;
      const int c = idx % cols;
      for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
          if (dr == 0 && dc == 0) continue;
          const int nr = r + dr;
          const int nc = c + dc;
          if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
          const int nIdx = nr * cols + nc;
          if (!revealed[nIdx] && !flagged[nIdx] && top < MAX_CELLS) {
            floodStack[top++] = nIdx;
          }
        }
      }
    }
  }
}

void MinesweeperActivity::toggleFlag(int row, int col) {
  const int idx = row * cols + col;
  if (revealed[idx]) return;
  flagged[idx] = !flagged[idx];
  flaggedCount += flagged[idx] ? 1 : -1;
}

void MinesweeperActivity::checkWin() {
  if (revealedSafeCount >= cols * rows - mineCount) {
    won = true;
  }
}

void MinesweeperActivity::onEnter() {
  Activity::onEnter();
  srand(millis());
  resetGame();
  requestUpdate();
}

void MinesweeperActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::GAMES_MENU);
    return;
  }

  if (gameOver || won) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      resetGame();
      requestUpdate();
    }
    return;
  }

  if (cursorRow == -1) {
    // Toolbar: two pills, "New Game" (col 0) and the Reveal/Flag mode toggle (col 1).
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      cursorCol = (cursorCol == 0) ? 1 : 0;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      cursorRow = 0;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (cursorCol == 0) {
        resetGame();
      } else {
        flagMode = !flagMode;
      }
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    cursorRow = (cursorRow == 0) ? -1 : cursorRow - 1;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    cursorRow = (cursorRow == rows - 1) ? -1 : cursorRow + 1;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (cursorCol > 0) {
      cursorCol--;
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (cursorCol < cols - 1) {
      cursorCol++;
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int idx = cursorRow * cols + cursorCol;
    if (flagMode) {
      toggleFlag(cursorRow, cursorCol);
    } else if (!flagged[idx]) {
      if (firstReveal) {
        firstReveal = false;
        placeMines(idx);
      }
      revealCell(cursorRow, cursorCol);
      if (!gameOver) {
        checkWin();
      }
    }
    requestUpdate();
  }
}

void MinesweeperActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MINESWEEPER_TITLE));

  const int remainingMines = mineCount - flaggedCount;
  char minesBuf[32];
  snprintf(minesBuf, sizeof(minesBuf), "%s %d", tr(STR_MINESWEEPER_MINES_LABEL), remainingMines);
  const int subHeaderY = metrics.topPadding + metrics.headerHeight;
  GUI.drawSubHeader(renderer, Rect{0, subHeaderY, pageWidth, SUBHEADER_HEIGHT}, minesBuf, nullptr);

  // Toolbar: two pills, "New" and the Reveal/Flag mode toggle, right-aligned
  // inline within the same subheader row as the mine count (rather than a
  // separate row below it, which used to collide with the subheader's bottom
  // hairline). Each pill is sized to its own label plus padding, so short
  // translations don't leave an oversized button; the mode pill uses the
  // wider of its two labels so it doesn't resize when the mode toggles.
  const int toolbarBtnH = TOOLBAR_BTN_HEIGHT;
  const int toolbarY = subHeaderY + (SUBHEADER_HEIGHT - toolbarBtnH) / 2;
  const int toolbarGap = 12;
  const int toolbarPadX = 14;

  const char* newGameLabel = tr(STR_MINESWEEPER_NEW_GAME);
  const char* revealLabel = tr(STR_MINESWEEPER_MODE_REVEAL);
  const char* flagLabel = tr(STR_MINESWEEPER_MODE_FLAG);
  const char* modeLabel = flagMode ? flagLabel : revealLabel;

  const int revealTextW = renderer.getTextWidth(SMALL_FONT_ID, revealLabel);
  const int flagTextW = renderer.getTextWidth(SMALL_FONT_ID, flagLabel);
  const int newGameBtnW = renderer.getTextWidth(SMALL_FONT_ID, newGameLabel) + toolbarPadX * 2;
  const int modeBtnW = (revealTextW > flagTextW ? revealTextW : flagTextW) + toolbarPadX * 2;

  const int modeX = pageWidth - metrics.contentSidePadding - modeBtnW;
  const int newGameX = modeX - toolbarGap - newGameBtnW;

  const bool onToolbar = (cursorRow == -1);
  const bool newGameSelected = onToolbar && cursorCol == 0;
  const bool modeSelected = onToolbar && cursorCol == 1;

  renderer.drawRoundedRect(newGameX, toolbarY, newGameBtnW, toolbarBtnH, 1, 6, true);
  if (newGameSelected) {
    renderer.fillRoundedRect(newGameX, toolbarY, newGameBtnW, toolbarBtnH, 6, Color::Black);
  }
  {
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, newGameLabel);
    const int textX = newGameX + (newGameBtnW - textW) / 2;
    const int textY = toolbarY + (toolbarBtnH - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
    renderer.drawText(SMALL_FONT_ID, textX, textY, newGameLabel, !newGameSelected);
  }

  renderer.drawRoundedRect(modeX, toolbarY, modeBtnW, toolbarBtnH, 1, 6, true);
  if (modeSelected) {
    renderer.fillRoundedRect(modeX, toolbarY, modeBtnW, toolbarBtnH, 6, Color::Black);
  }
  {
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, modeLabel);
    const int textX = modeX + (modeBtnW - textW) / 2;
    const int textY = toolbarY + (toolbarBtnH - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
    renderer.drawText(SMALL_FONT_ID, textX, textY, modeLabel, !modeSelected);
  }

  // Grid border + internal lines
  renderer.drawRect(gridOriginX, gridOriginY, cols * CELL_SIZE, rows * CELL_SIZE, 2, true);
  for (int i = 1; i < cols; i++) {
    renderer.drawLine(gridOriginX + i * CELL_SIZE, gridOriginY, gridOriginX + i * CELL_SIZE,
                       gridOriginY + rows * CELL_SIZE, 1, true);
  }
  for (int i = 1; i < rows; i++) {
    renderer.drawLine(gridOriginX, gridOriginY + i * CELL_SIZE, gridOriginX + cols * CELL_SIZE,
                       gridOriginY + i * CELL_SIZE, 1, true);
  }

  // Cell contents + cursor
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      const int idx = r * cols + c;
      const int cx = gridOriginX + c * CELL_SIZE;
      const int cy = gridOriginY + r * CELL_SIZE;

      // Shade every opened cell so a reveal with zero adjacent mines is
      // visibly different from an untouched cell (otherwise it looks like
      // the button press did nothing).
      if (revealed[idx] && !mine[idx]) {
        renderer.fillRectDither(cx + 2, cy + 2, CELL_SIZE - 4, CELL_SIZE - 4, Color::LightGray);
      }

      if (flagged[idx]) {
        const char* label = "F";
        const int textW = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
        const int textH = renderer.getLineHeight(UI_12_FONT_ID);
        renderer.drawText(UI_12_FONT_ID, cx + (CELL_SIZE - textW) / 2, cy + (CELL_SIZE - textH) / 2, label, true,
                           EpdFontFamily::BOLD);
      } else if (revealed[idx]) {
        if (mine[idx]) {
          const int pad = CELL_SIZE / 4;
          renderer.fillRect(cx + pad, cy + pad, CELL_SIZE - 2 * pad, CELL_SIZE - 2 * pad, true);
        } else if (adjacentCount[idx] > 0) {
          char buf[2] = {static_cast<char>('0' + adjacentCount[idx]), '\0'};
          const int textW = renderer.getTextWidth(UI_12_FONT_ID, buf, EpdFontFamily::BOLD);
          const int textH = renderer.getLineHeight(UI_12_FONT_ID);
          renderer.drawText(UI_12_FONT_ID, cx + (CELL_SIZE - textW) / 2, cy + (CELL_SIZE - textH) / 2, buf, true,
                             EpdFontFamily::BOLD);
        }
      } else if (gameOver && mine[idx]) {
        const int pad = CELL_SIZE / 4;
        renderer.fillRect(cx + pad, cy + pad, CELL_SIZE - 2 * pad, CELL_SIZE - 2 * pad, true);
      }

      // Cursor border drawn last so it stays visible over the reveal shading.
      if (!gameOver && !won && r == cursorRow && c == cursorCol) {
        renderer.drawRect(cx + 3, cy + 3, CELL_SIZE - 6, CELL_SIZE - 6, 2, true);
      }
    }
  }

  if (gameOver) {
    GUI.drawPopup(renderer, tr(STR_MINESWEEPER_BOOM));
  } else if (won) {
    GUI.drawPopup(renderer, tr(STR_MINESWEEPER_WIN));
  }

  const char* confirmLabel = (gameOver || won) ? tr(STR_MINESWEEPER_NEW_GAME) : nullptr;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, nullptr, nullptr);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
