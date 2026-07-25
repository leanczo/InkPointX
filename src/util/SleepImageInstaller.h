#pragma once

#include <string>

namespace SleepImageInstaller {

constexpr const char* INSTALLED_IMAGE_PATH = "/sleep.bmp";

// Installs a BMP, JPEG, or PNG from the SD card as the sleep/lock-screen
// image. JPEG and PNG conversion is performed on the device.
bool install(const std::string& sourcePath, bool crop);

}  // namespace SleepImageInstaller
