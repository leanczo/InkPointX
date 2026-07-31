#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  // Tag type: acquire without blocking. locked() reports whether it took the
  // mutex — the caller must check before relying on the lock.
  struct Try {};

  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  explicit RenderLock(Try);
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  bool locked() const { return isLocked; }
  static bool peek();
};
