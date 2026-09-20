#pragma once

extern "C" {
void lvgl_lock(void);
void lvgl_unlock(void);
}

namespace xiaozhi {

/* Small RAII boundary used by worker-facing display update functions. */
class LvglLockGuard {
public:
  LvglLockGuard() { lvgl_lock(); }
  ~LvglLockGuard() { lvgl_unlock(); }

  LvglLockGuard(const LvglLockGuard &) = delete;
  LvglLockGuard &operator=(const LvglLockGuard &) = delete;
};

} // namespace xiaozhi
