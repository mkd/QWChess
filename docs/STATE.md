# QwenChess — current state

**Milestone:** `CPP20-3B` (done this commit) — the clock now exposes native
`std::chrono::steady_clock` **time points** (no invented nonnegative epoch, no
negative-error scalar); the scaffold entry point uses coherent C++ stream output.
Phase 3 of the C++20 workstream is **complete** after this; there is no 3C.

**Last verified commit:** `dcfa2eb` (CPP20-3A) is the parent; this commit is CPP20-3B.

## What this checkpoint did
- **Clock contract** (`platform/clock.{hpp,cpp}`): `using SteadyClock =
  std::chrono::steady_clock; using TimePoint = SteadyClock::time_point;
  [[nodiscard]] TimePoint clock_now() noexcept;` implemented as a direct return of
  `SteadyClock::now()`, with `static_assert(SteadyClock::is_steady)`. The old
  `clock_now_ns()/clock_now_us()` (which treated a possibly-negative epoch count as a
  clock error) and their only caller (the scaffold test) are removed. Contract
  documented in the header: local elapsed-time / deadlines only (not civil timestamps);
  compare directly, elapsed = `end - start`; equal readings valid; no resolution/epoch
  promised; a default-constructed point is not an error; no negative-return convention.
- **Scaffold output** (`engine/main.cpp`): coherent C++ streams with direct includes and
  `'\n'`; keeps the honest "scaffold, not a playable engine" label + the constants.
  `NO_SQUARE` (byte-sized) is streamed through an explicit integer conversion so it
  prints `64`, not a character; the width diagnostic is labelled **pointer width** and
  computed as `sizeof(void*) * CHAR_BIT`. No logging framework / UCI loop / format
  requirement introduced.
- **Tests/docs:** the scaffold clock test now checks the return type, `noexcept`, and
  the steady property at compile time plus a bounded nondecreasing / nonnegative-elapsed
  runtime sequence. `STATE`/`PLAN` corrected and rescheduled (below).

## Roadmap correction (the functional track resumes)
T005 **cp1 (non-castling pseudo-legal movegen) is done** — it is not "no T005 work".
What remains in M1: cp2 (non-castling **legal** filtering), cp3 (castling), cp4
(perft + independent differential = M1 exit). The C++20 order is now
**CPP20-3B → T005 cp2/cp3/cp4 → T006/T007 (M2, first playable) → CPP20-4** (build
profiles + same-ISA comparison, moved to after M2, before M6 platform tuning).
CPP20-4's requirements are retained, not deleted.

## Latest checks (host: x86-64, 8 cores, 126 GiB, Debian 13, GCC 16.1.0)
- `check-cpp20`: **green** (23 files, no non-C++20 constructs).
- release + debug builds: **0 warnings**; engine builds in all three configs.
- `make test CONFIG=release`: **194 PASS groups, 0 FAIL**, exit 0.
- `make test CONFIG=debug`: **194 PASS groups, 0 FAIL**, exit 0.
- Sanitizer: instrumented build + all **15 suites pass via the documented
  `LD_PRELOAD=libasan.so.8` + `detect_leaks=1` workaround** (0 ASan/UBSan/leak
  diagnostics). The **ordinary `make test-sanitize` target fails on this host**
  (`/etc/ld.so.preload` → `libenable-3fg-drag.so` VM tool breaks ASan loader order) —
  reported separately; the workaround run is the valid code result.
- **Clang: not on the PATH** (normal lookup) → reported **unrun**; no toolchain
  installed. Apple Silicon/AArch64 and RV64 remain unverified.

## Blockers
None for CPP20-3B. The ASan-launcher and missing-Clang items are environmental, not
code defects.

## Next (exactly one task)
**T005 cp2 — non-castling legal move generation**: filter each pseudo-legal candidate
by the *original mover's* king safety after `pos_make_move`/`pos_unmake_move`,
including geometric attacks from pinned enemy pieces; retain all king-safe moves
(blocks, sacrifices, EP/promotion evasions); no SEE pruning. (cp1 pseudo-legal gen is
the existing, tested `src/movegen/`.)
