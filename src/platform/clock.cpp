#include "platform/clock.hpp"
#include <chrono>

std::int64_t qwc_clock_now_ns() {
  /* std::chrono::steady_clock is the standard library's monotonic,
   * wall-clock-independent clock. It is monotonic by definition (a later now()
   * is never earlier) and its epoch is implementation-defined; the standard does
   * NOT guarantee that the raw time_since_epoch() count is nonnegative. We keep
   * the documented API contract (a negative count signals a clock error / "time
   * unknown", see clock.hpp) defensively rather than assuming it away. The exact
   * epoch / monotonicity / error-signaling contract is a CPP20-3 platform design
   * review item; the measured behavior here is unchanged in this checkpoint. */
  auto const since = std::chrono::steady_clock::now().time_since_epoch();
  auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(since).count();
  return static_cast<std::int64_t>(ns);
}
