/* QwenChess test harness (test-only, never shipped).
 *
 * Plain, always-active checks (NOT #ifndef NDEBUG-guarded) so assertions hold
 * even in a release build. Failure messages name the primitive and show the
 * expected vs actual value so a failing case is reproducible. */
#ifndef QWC_TESTS_UTIL_H
#define QWC_TESTS_UTIL_H

#include <stdio.h>
#include <stdint.h>
#include "core/types.h"

static int qwc_cases = 0;
static int qwc_fails = 0;
static const char *qwc_name = "";
static int qwc_fail_budget = 25;   /* cap on printed failures per run */

static void qwc_begin(const char *name) {
  qwc_name = name; qwc_cases = 0; qwc_fails = 0; qwc_fail_budget = 25;
}

/* Returns nonzero if any check in the current group failed. */
static int qwc_end(void) {
  int failed = qwc_fails > 0;
  printf("%-22s %8d checks, %3d failed -> %s\n",
         qwc_name, qwc_cases, qwc_fails, failed ? "FAIL" : "PASS");
  return failed;
}

#define QWC_TRUE(cond, label) do { \
  qwc_cases++; \
  if (!(cond)) { qwc_fails++; \
    if (qwc_fails <= qwc_fail_budget) \
      printf("  FAIL %s:%d %s: !(%s)\n", __FILE__, __LINE__, label, #cond); } \
} while (0)

#define QWC_EQ_I(got, exp, label) do { \
  qwc_cases++; int _g = (got), _e = (exp); \
  if (_g != _e) { qwc_fails++; \
    if (qwc_fails <= qwc_fail_budget) \
      printf("  FAIL %s:%d  %s: got %d exp %d\n", __FILE__, __LINE__, label, _g, _e); } \
} while (0)

#define QWC_EQ_U64(got, exp, label) do { \
  qwc_cases++; u64 _g = (got), _e = (exp); \
  if (_g != _e) { qwc_fails++; \
    if (qwc_fails <= qwc_fail_budget) \
      printf("  FAIL %s:%d  %s: got 0x%016llx exp 0x%016llx\n", __FILE__, __LINE__, \
             label, (unsigned long long)_g, (unsigned long long)_e); } \
} while (0)

#endif /* QWC_TESTS_UTIL_H */
