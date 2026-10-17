/* QwenChess platform: monotonic clock.
 * Monotonic (never decreases); wall-clock independent. Sub-nanosecond calls
 * may return the same value. Returns nanoseconds; a negative value signals a
 * clock error (callers treat it as "time unknown"). Backed by the standard
 * library's std::chrono::steady_clock. */
#ifndef QWC_PLATFORM_CLOCK_HPP
#define QWC_PLATFORM_CLOCK_HPP

#include <cstdint>

namespace qwc {

std::int64_t clock_now_ns();

inline std::int64_t clock_now_us() {
  std::int64_t ns = clock_now_ns();
  return ns < 0 ? -1 : ns / 1000;
}

}  // namespace qwc

#endif /* QWC_PLATFORM_CLOCK_HPP */
