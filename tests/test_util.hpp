/* QwenChess test harness (test-only, never shipped).
 *
 * Plain, always-active checks (NOT #ifndef NDEBUG-guarded) so assertions hold
 * even in a release build. Failure messages name the primitive and show the
 * expected vs actual value so a failing case is reproducible. */
#ifndef QWC_TESTS_UTIL_HPP
#define QWC_TESTS_UTIL_HPP

#include <cstdio>
#include <cstdint>
#include <type_traits>
#include "core/types.hpp"

/* Convert a domain value or an integer to its int form for assertion comparison.
 * A scoped enum (Piece/PieceType/Color/MoveFlag) is taken through qwc::to_under(); any
 * other (already-integer) type is a plain cast. This is the single, documented
 * typed->raw boundary for the check macros below, so call sites may keep values
 * typed instead of scattering qwc::to_under() at every assertion. */
template <typename T>
static inline int qwc_as_int(T v) {
  if constexpr (std::is_enum_v<T>) return qwc::to_under(v);
  else return (int)v;
}
template <typename T>
static inline qwc::u64 qwc_as_u64(T v) {
  if constexpr (std::is_enum_v<T>) return qwc::to_under(v);
  else return (qwc::u64)v;
}

static int qwc_cases = 0;
static int qwc_fails = 0;
static const char *qwc_name = "";
static int qwc_fail_budget = 25;   /* cap on printed failures per run */

static void qwc_begin(const char *name) {
  qwc_name = name; qwc_cases = 0; qwc_fails = 0; qwc_fail_budget = 25;
}

/* Returns nonzero if any check in the current group failed. */
static int qwc_end() {
  int failed = qwc_fails > 0;
  std::printf("%-22s %8d checks, %3d failed -> %s\n",
         qwc_name, qwc_cases, qwc_fails, failed ? "FAIL" : "PASS");
  return failed;
}

#define QWC_TRUE(cond, label) do { \
  qwc_cases++; \
  if (!(cond)) { qwc_fails++; \
    if (qwc_fails <= qwc_fail_budget) \
      std::printf("  FAIL %s:%d %s: !(%s)\n", __FILE__, __LINE__, label, #cond); } \
} while (0)

#define QWC_EQ_I(got, exp, label) do { \
  qwc_cases++; int _g = qwc_as_int(got), _e = qwc_as_int(exp); \
  if (_g != _e) { qwc_fails++; \
    if (qwc_fails <= qwc_fail_budget) \
      std::printf("  FAIL %s:%d  %s: got %d exp %d\n", __FILE__, __LINE__, label, _g, _e); } \
} while (0)

#define QWC_EQ_U64(got, exp, label) do { \
  qwc_cases++; qwc::u64 _g = qwc_as_u64(got), _e = qwc_as_u64(exp); \
  if (_g != _e) { qwc_fails++; \
    if (qwc_fails <= qwc_fail_budget) \
      std::printf("  FAIL %s:%d  %s: got 0x%016llx exp 0x%016llx\n", __FILE__, __LINE__, \
             label, (unsigned long long)_g, (unsigned long long)_e); } \
} while (0)

#endif /* QWC_TESTS_UTIL_HPP */
