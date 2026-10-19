/* QwenChess platform: monotonic clock.
 *
 * Exposes the standard library's monotonic, wall-clock-independent
 * std::chrono::steady_clock directly, as native time points. There is no invented
 * nonnegative epoch and no integer scalar return here: a steady clock's epoch is
 * implementation-defined and arbitrary, so negativity is not an error indication.
 *
 * Contract (the one place this is pinned; see also platform/clock.cpp):
 *  - clock_now() returns the current steady time point. steady_clock is steady
 *    (a later now() is never earlier); its epoch is arbitrary. There is NO
 *    negative / error return convention: a time point is never "in error", and a
 *    default-constructed time point is not an error or an absent-deadline sentinel.
 *  - Time points measure LOCAL elapsed time and express deadlines; they are not
 *    civil timestamps. They are totally ordered, so compare them directly and
 *    compute an elapsed time as `end - start` (a duration). Equal consecutive
 *    readings are valid. Neither a particular resolution (e.g. nanoseconds) nor
 *    any specific epoch is promised.
 *  - Convert elapsed durations to protocol / reporting units at their eventual
 *    boundary (M2 time management), not by converting absolute epochs here.
 */
#ifndef QWC_PLATFORM_CLOCK_HPP
#define QWC_PLATFORM_CLOCK_HPP

#include <chrono>

namespace qwc {

/* The project's monotonic clock: the standard library's steady_clock, exposed
 * directly. Its is_steady guarantee is part of the API contract (a later now()
 * is never earlier); it is asserted here so any steady_clock substitution that
 * lost monotonicity fails at compile time. */
using SteadyClock = std::chrono::steady_clock;
using TimePoint   = SteadyClock::time_point;

static_assert(SteadyClock::is_steady, "steady_clock must be steady (monotonic)");

/* The current monotonic time point. A direct pass-through of steady_clock::now().
 * Use it for elapsed-time / deadline measurement; it is not a civil timestamp. */
[[nodiscard]] TimePoint clock_now() noexcept;

}  // namespace qwc

#endif /* QWC_PLATFORM_CLOCK_HPP */
