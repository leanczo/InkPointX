#pragma once

#include <string>

#include "activities/Activity.h"

// Network fetches are synchronous (block the main loop behind a "Loading"
// state, same as FontDownloadActivity) instead of the background-FreeRTOS-
// task pattern the source app used — activities in this codebase are not
// supposed to spawn their own tasks (see docs/activity-manager.md).
class WeatherActivity final : public Activity {
 public:
  enum class WeatherState { Init, SelectCity, Loading, ShowWeather };

 private:
  WeatherState state = WeatherState::Init;
  int selectedCityIndex = 0;
  bool weatherLoaded = false;
  double temp = 0.0;
  double windspeed = 0.0;
  int weatherCode = 0;
  bool isDay = true;
  std::string timeStr;
  std::string cityName;
  std::string errorMessage;

  void performFetch();
  void doWeatherFetch();

 public:
  explicit WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Weather", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
