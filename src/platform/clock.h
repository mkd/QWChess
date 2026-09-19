/* QwenChess platform: monotonic clock.
 * Monotonic (never decreases); wall-clock independent. Sub-nanosecond calls
 * may return the same value. Returns nanoseconds; a negative value signals a
 * clock error (callers treat it as "time unknown"). */
#ifndef QWC_PLATFORM_CLOCK_H
#define QWC_PLATFORM_CLOCK_H

#include <stdint.h>

int64_t qwc_clock_now_ns(void);

static inline int64_t qwc_clock_now_us(void) {
  int64_t ns = qwc_clock_now_ns();
  return ns < 0 ? -1 : ns / 1000;
}

#endif /* QWC_PLATFORM_CLOCK_H */
