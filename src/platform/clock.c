#define _POSIX_C_SOURCE 199309L
#include "platform/clock.h"
#include <time.h>

int64_t qwc_clock_now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return -1;
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}
