#include "platform/clock.hpp"

#include <chrono>

namespace qwc {

/* A direct return of steady_clock::now(). steady_clock is monotonic by definition
 * (a later now() is never earlier) and its epoch is implementation-defined and
 * arbitrary; the returned time point is used for local elapsed-time measurement and
 * deadline arithmetic, never interpreted as a civil timestamp, a signed count, or an
 * error indicator. (There is deliberately no conversion to an integer epoch here.) */
TimePoint clock_now() noexcept {
  return SteadyClock::now();
}

}  // namespace qwc
