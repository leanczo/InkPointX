#pragma once

#include <freertos/FreeRTOS.h>

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  explicit RenderLock();
  explicit RenderLock(TickType_t timeoutTicks);
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  bool locked() const { return isLocked; }
  static bool peek();
};

// Temporarily releases one recursive level of ActivityManager's render mutex.
// ActivityManager::loop() owns that level while calling Activity::loop(); a
// long synchronous operation can use this scope to let the render task paint
// progress updates, then regain the loop's state lock before returning.
class ScopedRenderUnlock {
  bool wasLocked = false;

 public:
  ScopedRenderUnlock();
  ScopedRenderUnlock(const ScopedRenderUnlock&) = delete;
  ScopedRenderUnlock& operator=(const ScopedRenderUnlock&) = delete;
  ~ScopedRenderUnlock();
};
