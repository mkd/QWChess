# QwenChess — State

**Milestone:** the **CPP20 modernization workstream** (a language/refactor track, not a
functionality milestone). This checkpoint is the **CPP20-2A closeout**: the scoped-enum
commit (`70f6d2b`) is mapped to 2A, and this commit lands the closeout — the Makefile
`test` fix, the compile-time type/layout assertions, the comment/`size_t` corrections,
the engineering policy (ARCHITECTURE.md), the requirements register (REQUIREMENTS.md)
and the 4-phase roadmap reconciliation. **M1 board work (T001–T005 cp1) is preserved** —
behavior and test evidence are identical, only types/docs/build changed.
**Baseline (last verified commit before this checkpoint):** `70f6d2b` (CPP20-2 scoped
enums). Git identity `mkd`/`claudiomkd@gmail.com`.

## This checkpoint commits (CPP20-2A closeout)
- **`Makefile`:** `test` honors `CONFIG` (release/debug/sanitize; an unknown value is a
  **hard error**, `$(error)`); `test-run`/`all-config` are `.PHONY`; help text corrected;
  `build/cpp20/<config>/` trees unchanged.
- **`scaffold_test`:** compile-time asserts — the four domain enums are the exact scoped
  types (`is_enum` + **no implicit conversion** + exact `underlying_type` u8/u8/u8/u32),
  every value/sentinel frozen, and `Position`/`StateInfo`/`MoveDelta`/`SquareEdit` are
  trivially-copyable + standard-layout with the x86-64 size/alignment baseline (248/8,
  48/8, 20/4, 3/1, ABI-guarded).
- **Type/portability:** `size_t`→`std::size_t` + direct `<cstddef>`; comment corrections
  (`<bit>` zero→bit width not UB, "null move", `*_NB` a design choice, `numeric_limits` a
  facility, steady_clock epoch not guaranteed nonnegative). No behavior change.
- **Docs:** ARCHITECTURE.md Engineering policy; REQUIREMENTS.md register (BUILD-1/2,
  PERF-1, UCI-1, CONSOLE-1, SEARCH-1, TB-1); PLAN.md 4-phase reconciliation + corrected
  T005 cp2/cp3; AGENTS.md build commands + policy + REQUIREMENTS link.

## Checks (this host, GCC/g++ — **newly run this checkpoint**)
- `make check-cpp20` → **23 files**, exit 0.
- `make clean && make` (release) → **0 errors, 0 warnings**; `qwenchess` scaffold runs,
  prints constants, exit 0.
- `make test` (release, default `CONFIG`) → **15/15, 184 PASS, 0 fail**, exit 0.
- `make test CONFIG=debug` → **15/15, 184 PASS, 0 fail**, exit 0.
- `make test CONFIG=sanitize` → instrumented binaries **build 0 warnings**, but the
  **normal run fails on this host** (ASan "runtime does not come first… preload with
  LD_PRELOAD", exit 2). Under a **documented `LD_PRELOAD=libasan.so.8` workaround, all
  15 execute and pass** — this is a workaround, **not** ordinary sanitizer coverage.
  (The CPP20-2 STATE's "sanitize runs clean (exit 0)" claim is **retracted**: it was the
  pre-fix `test` silently running release.)
- portable-bits (`-DQWC_FORCE_PORTABLE_BITS=1`): bitboard-dependent tests build 0
  warnings and pass. `git diff --check` clean.

## Blockers / open
- **Clang not installed** here; only GCC/g++ verified (carried to CPP20-3/4).
- `.gitignore` (last touched in CPP20-1 `a05f3bd`) covers `build/`/`networks/`/
  `reference/`/cruft; I did **not** modify it this checkpoint and the known **build-only
  .gitignore gap is carried forward, not closed**. No build artifact is at risk of
  committing in the current tree.
- `reference/` (M3 oracle) and the match runner (M5) are not made; neither blocks 2B.

## Exactly one next task
**CPP20-2B — project namespace, consistent naming, remaining typed constants + fixed
storage** (`std::array`), preserving behavior and documenting the migrated surface. No
language-standard increase, no new CPU flags, no ISA. M1 board work resumes at **T005
cp2** after 2B.
