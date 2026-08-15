#include "SnakeActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char SNAKE_APPS_DIR[] = "/apps/snake";
constexpr char SNAKE_HIGH_SCORE_FILE[] = "/apps/snake/highscore.txt";
}  // namespace

void SnakeActivity::loadHighScore() {
  highScore = 0;
  Storage.recoverInterruptedWrite(SNAKE_HIGH_SCORE_FILE);
  if (!Storage.exists(SNAKE_HIGH_SCORE_FILE)) return;
  const String content = Storage.readFile(SNAKE_HIGH_SCORE_FILE);
  if (content.isEmpty()) return;
  highScore = std::max(0, static_cast<int>(content.toInt()));
}

void SnakeActivity::saveHighScore() const {
  Storage.mkdir(SNAKE_APPS_DIR);
  Storage.writeFile(SNAKE_HIGH_SCORE_FILE, String(highScore));
}

void SnakeActivity::computeGrid() {
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
    // Defensive only: CELL_SIZE keeps cols*rows well under MAX_CELLS on
    // this display, but never let a future metrics change overflow `body`.
    rows = MAX_CELLS / cols;
  }

  const int gridPixelW = cols * CELL_SIZE;
  const int gridPixelH = rows * CELL_SIZE;
  gridOriginX = (pageWidth - gridPixelW) / 2;
  gridOriginY = contentTop + (contentHeight - gridPixelH) / 2;
}

bool SnakeActivity::isOccupied(int x, int y, int checkLen) const {
  for (int i = 0; i < checkLen; i++) {
    if (body[i].x == x && body[i].y == y) return true;
  }
  return false;
}

void SnakeActivity::placeFood() {
  const int totalCells = cols * rows;
  if (snakeLength >= totalCells) {
    // Board full: nowhere left to place food.
    gameOver = true;
    return;
  }
  int x, y;
  do {
    x = rand() % cols;
    y = rand() % rows;
  } while (isOccupied(x, y, snakeLength));
  food = SnakeCell{static_cast<int8_t>(x), static_cast<int8_t>(y)};
}

void SnakeActivity::resetGame() {
  computeGrid();
  snakeLength = 3;
  direction = SnakeDirection::Right;
  pendingDirection = SnakeDirection::Right;

  const int startX = cols / 2;
  const int startY = rows / 2;
  for (int i = 0; i < snakeLength; i++) {
    body[i] = SnakeCell{static_cast<int8_t>(startX - i), static_cast<int8_t>(startY)};
  }

  score = 0;
  gameOver = false;
  lastTickMs = millis();
  placeFood();
}

void SnakeActivity::tick() {
  // A same-tick Up-then-Down (or Left-then-Right) key sequence could queue a
  // direct reversal into the snake's own neck; drop it instead of committing.
  const bool isReversal = (pendingDirection == SnakeDirection::Up && direction == SnakeDirection::Down) ||
                          (pendingDirection == SnakeDirection::Down && direction == SnakeDirection::Up) ||
                          (pendingDirection == SnakeDirection::Left && direction == SnakeDirection::Right) ||
                          (pendingDirection == SnakeDirection::Right && direction == SnakeDirection::Left);
  if (!isReversal) {
    direction = pendingDirection;
  } else {
    pendingDirection = direction;
  }

  SnakeCell newHead = body[0];
  switch (direction) {
    case SnakeDirection::Up:
      newHead.y--;
      break;
    case SnakeDirection::Down:
      newHead.y++;
      break;
    case SnakeDirection::Left:
      newHead.x--;
      break;
    case SnakeDirection::Right:
      newHead.x++;
      break;
  }

  if (newHead.x < 0 || newHead.x >= cols || newHead.y < 0 || newHead.y >= rows) {
    endGame();
    return;
  }

  const bool willGrow = (newHead.x == food.x && newHead.y == food.y);
  // The tail cell vacates this tick unless the snake is growing, so it's safe
  // to move onto it; excluding it from the collision check is what lets the
  // snake curl through its own former tail position.
  const int checkLen = willGrow ? snakeLength : snakeLength - 1;
  if (isOccupied(newHead.x, newHead.y, checkLen)) {
    endGame();
    return;
  }

  int newLen = willGrow ? snakeLength + 1 : snakeLength;
  if (newLen > MAX_CELLS) newLen = MAX_CELLS;
  for (int i = newLen - 1; i > 0; i--) {
    body[i] = body[i - 1];
  }
  body[0] = newHead;
  snakeLength = newLen;

  if (willGrow) {
    score++;
    placeFood();
  }
}

void SnakeActivity::endGame() {
  gameOver = true;
  if (score > highScore) {
    highScore = score;
    saveHighScore();
  }
}

void SnakeActivity::onEnter() {
  Activity::onEnter();
  srand(millis());
  loadHighScore();
  resetGame();
  requestUpdate();
}

void SnakeActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::GAMES_MENU);
    return;
  }

  if (gameOver) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      resetGame();
      requestUpdate();
      return;
    }
    // Reuses the Left slot, which moves the snake during play but does
    // nothing once it's dead - the confirmation dialog protects the saved
    // record from an accidental press.
    if (highScore > 0 && mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      auto handler = [this](const ActivityResult& res) {
        if (!res.isCancelled) {
          highScore = 0;
          saveHighScore();
        }
        requestUpdate();
      };
      startActivityForResult(
          makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_SNAKE_RESET_RECORD_TITLE),
                                                   tr(STR_SNAKE_RESET_RECORD_BODY)),
          handler);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up) && pendingDirection != SnakeDirection::Down) {
    pendingDirection = SnakeDirection::Up;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) && pendingDirection != SnakeDirection::Up) {
    pendingDirection = SnakeDirection::Down;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) && pendingDirection != SnakeDirection::Right) {
    pendingDirection = SnakeDirection::Left;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right) && pendingDirection != SnakeDirection::Left) {
    pendingDirection = SnakeDirection::Right;
  }

  const unsigned long now = millis();
  if (now - lastTickMs >= TICK_MS) {
    lastTickMs = now;
    tick();
    requestUpdate();
  }
}

void SnakeActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SNAKE));

  char scoreBuf[32];
  snprintf(scoreBuf, sizeof(scoreBuf), "%s: %d", tr(STR_SCORE), score);
  char recordBuf[32];
  snprintf(recordBuf, sizeof(recordBuf), "%s: %d", tr(STR_SNAKE_HIGH_SCORE), highScore);
  const int subHeaderY = metrics.topPadding + metrics.headerHeight;
  GUI.drawSubHeader(renderer, Rect{0, subHeaderY, pageWidth, SUBHEADER_HEIGHT}, scoreBuf, recordBuf);

  renderer.drawRect(gridOriginX, gridOriginY, cols * CELL_SIZE, rows * CELL_SIZE, 1, true);

  for (int i = 0; i < snakeLength; i++) {
    const int x = gridOriginX + body[i].x * CELL_SIZE;
    const int y = gridOriginY + body[i].y * CELL_SIZE;
    if (i == 0) {
      renderer.fillRect(x + 1, y + 1, CELL_SIZE - 2, CELL_SIZE - 2, true);
    } else {
      renderer.fillRect(x + 2, y + 2, CELL_SIZE - 4, CELL_SIZE - 4, true);
    }
  }

  // Hollow square keeps the food visually distinct from the solid snake body.
  {
    const int x = gridOriginX + food.x * CELL_SIZE;
    const int y = gridOriginY + food.y * CELL_SIZE;
    renderer.drawRect(x + 2, y + 2, CELL_SIZE - 4, CELL_SIZE - 4, 2, true);
  }

  if (gameOver) {
    char overBuf[48];
    snprintf(overBuf, sizeof(overBuf), "%s %d", tr(STR_SNAKE_GAME_OVER), score);
    GUI.drawPopup(renderer, overBuf);
  }

  const char* confirmLabel = gameOver ? tr(STR_SNAKE_RESTART) : nullptr;
  // While playing, Left/Right move the snake, so they show as arrow hints.
  // Once dead, movement no longer applies, and Left is repurposed for the
  // record-reset shortcut (only offered once a record actually exists).
  const char* leftLabel = gameOver ? (highScore > 0 ? tr(STR_SNAKE_RESET_RECORD) : nullptr) : tr(STR_DIR_LEFT);
  const char* rightLabel = gameOver ? nullptr : tr(STR_DIR_RIGHT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, leftLabel, rightLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
