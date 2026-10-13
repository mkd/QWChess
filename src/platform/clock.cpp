#include "platform/clock.hpp"
#include <chrono>

std::int64_t qwc_clock_now_ns() {
  /* std::chrono::steady_clock is the portable, monotonic, wall-clock-independent
   * clock the standard library provides. It is measured against a fixed epoch and
   * never fails, so the documented "negative = clock error" contract is preserved
   * (it is simply not produced on a working host). */
  auto const since = std::chrono::steady_clock::now().time_since_epoch();
  auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(since).count();
  return static_cast<std::int64_t>(ns);
}
