#pragma once

#include <string>

#include "activities/Activity.h"

enum class ConverterState { CategoryPicker, FromUnitPicker, ToUnitPicker, ValueEntry, Result };

// Fully offline -- no HttpDownloader, no WiFi, no onExit heap-defrag reboot.
// Every unit is defined as an affine transform to a per-category base unit
// (toBase(raw) = raw*factor + offset), which is enough to cover linear units
// (length/weight/volume, offset=0) and temperature (Celsius as base, with
// Fahrenheit/Kelvin's own offset) through one shared conversion function --
// no special-casing per category.
class UnitConverterActivity final : public Activity {
 private:
  ConverterState state = ConverterState::CategoryPicker;

  int selectedCategory = 0;
  int selectedFromUnit = 0;
  int selectedToUnit = 0;
  int pickerIndex = 0;  // reused across CategoryPicker/FromUnitPicker/ToUnitPicker

  // Numeric keypad, same shape as CalculatorActivity's CALC_GRID (a 2D array
  // of labels with do-while-skipped blank cells), restricted to digits/./-
  // plus Del and OK since this only ever builds a plain decimal number.
  int selRow = 0;
  int selCol = 0;
  std::string inputValue;
  double resultValue = 0.0;

  void handleKeypadConfirm();
  void computeResult();

 public:
  explicit UnitConverterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("UnitConverter", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
