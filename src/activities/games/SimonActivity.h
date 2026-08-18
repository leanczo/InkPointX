#pragma once

#include <cstdint>

#include "activities/Activity.h"

// Classic Simon: 4 quadrants (Up/Right/Down/Left, matching the physical D-pad
// 1:1 so no extra input scheme is needed) light up in a growing sequence that
// the player must repeat. All state transitions are driven by non-blocking
// millis() timers polled from loop(), same pattern as SnakeActivity's TICK_MS
// tick — there is no per-frame animation, only discrete e-ink-friendly redraws.
enum class SimonState : uint8_t { Idle, ShowingSequence, RoundCorrect, PlayerInput, GameOver };

class SimonActivity final : public Activity {
 private:
  static constexpr int MAX_SEQUENCE = 64;  // far beyond any realistically reachable round count
  static constexpr unsigned long SEQ_ON_MS = 900;
  static constexpr unsigned long SEQ_OFF_MS = 500;
  static constexpr unsigned long ROUND_CORRECT_MS = 1800;  // time the "Correct!" popup stays up before the next round

  int8_t sequence[MAX_SEQUENCE]{};  // each entry is a quadrant index 0-3
  int sequenceLength = 0;

  SimonState state = SimonState::Idle;
  unsigned long phaseStartMs = 0;

  int showStep = 0;        // index into sequence[] while ShowingSequence
  bool showingOn = false;  // current on/off half of the flash for showStep

  int inputIndex = 0;  // how many correct presses so far this round
  // Quadrant currently lit as press feedback, -1 = none. Stays lit until the
  // next press or state change overwrites it.
  int flashIndex = -1;

  static int quadrantForButton(MappedInputManager::Button button);
  void resetGame();
  void beginNewSequence();  // sequenceLength = 0, add one step, start playback
  void startShowingSequence();
  void appendRandomStep();

 public:
  explicit SimonActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Simon", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
