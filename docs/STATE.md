# QwenChess — State

**Milestone:** M1 (board correctness) is the functional track; this checkpoint is
the **CPP20-1 language migration**, now complete and verified. All shipped sources
are now **C++20** (`.cpp`/`.hpp`, `g++`, `build/cpp20/`). The M1 board work (T001–T005)
is preserved as-is; the migration is a language-only refactor that keeps the compact
representation, behavior and test evidence identical.
**Baseline (last verified commit):** `93d3c77` (T005 cp1: non-castling pseudo-legal
movegen) — the pre-migration **C17** baseline. This checkpoint builds on it.
Git identity `mkd`/`claudiomkd@gmail.com`.

## This checkpoint commits (CPP20-1 — C17 → C++20 language migration)
- **Renames:** every engine/core/platform/position/movegen `.c`→`.cpp` and `.h`→`.hpp`;
  all project includes updated. **`Makefile`:** `CC`→`CXX`, `CFLAGS`→`CXXFLAGS`,
  `-std=c17`→`-std=c++20`, `CXX=g++` (clang absent on this host; `CXX=clang++` remains
  the opt-in), dropped `-Wstrict-prototypes`/`-Wmissing-prototypes` (C-only), added
  `-Wno-implicit-fallthrough` (C++20 `[[fallthrough]]` idiom) and `-Wimplicit-fallthrough`,
  routed objects to `build/cpp20/<config>/`, renamed `check-c17`→`check-cpp20`
  (`-std=c++20 -pedantic-errors`).
- **Language cleanup (all 23 translation units):** `_Static_assert`→`static_assert`,
  `NULL`→`nullptr`, C headers→C++ standard headers, fixed-width `typedef`→`using`
  with `std::` qualification, `typedef struct … x;`→`struct …;`, `std::`-qualified
  `memcpy`/`memset`/`printf`/etc., the `Position`/`StateInfo`/`MoveDelta`/`SquareEdit`
  aggregates kept **plain** (no constructors), `POS_HM_MAX`/`FEN_BUF_SIZE`/`BB_FULL`→
  `constexpr`, bitboard count/scan switched to **`<bit>`** (`std::popcount`/
  `countr_zero`/`countl_zero`) with the forced portable fallback retained, the clock
  modernized to **`std::chrono::steady_clock`**, and the five enum/non-enum ternary
  warnings fixed at the representation boundary (explicit sentinel casts).
- **Docs:** AGENTS.md flipped to C++20 (g++/CXXFLAGS/check-cpp20/`build/cpp20/` + a
  C++20-conventions section); PLAN.md adds the CPP20 workstream; ARCHITECTURE/README
  no longer describe the codebase as C17.

## Checks (this host, re-verified after the conversion)
- `make check-cpp20` → **23 files**, no non-C++20 constructs. **(exit 0)**
- `make` (release) → builds, **0 warnings**, no `-Wstrict-prototypes`/C-only errors. **(exit 0)**
- `make test` (release) → **15/15** binaries pass, 184 PASS lines, 0 fail. **(exit 0)**
- `make debug` + `make test` → **15/15** pass, 0 fail. **(exit 0)**
- Struct size/alignment **unchanged** vs the C baseline: Position 248/8, StateInfo 48/8,
  MoveDelta 20/4, SquareEdit 3/1 (verified with a C++ size probe).
- **Portable-bit path** (`QWC_FORCE_PORTABLE_BITS=1`) → exercised and green (same
  15/15 class of results as the `<bit>` path).
- **`make test-sanitize` — host-limited, not a code defect:** the ordinary target
  aborts at ASan startup (`ASan runtime does not come first in initial library list`)
  because `/etc/ld.so.preload` injects a setuid non-dpkg library ahead of `libasan`
  (same root cause as T004). **Alternate launch** `LD_PRELOAD=$(g++ -print-file-name=libasan.so)`
  → re-verify 15/15 under ASan+UBSan before committing (see Blockers).

## Blockers / open
- **Sanitizer normal target is runner-limited on this host** (env, not build): use the
  documented `LD_PRELOAD` alternate launch; re-ran to confirm 15/15 green. Does not
  block CPP20-2.
- **Clang is not installed** here (`clang++` absent); only GCC/g++ was available. The
  cross-compiler Clang build must be verified on a Clang-capable host (carried to CPP20-3).
- `reference/` SF19 checkout (M3 oracle) and the match runner (M5) are not made; neither
  blocks CPP20-2.

## Exactly one next task
**CPP20-2 — scoped enums with explicit underlying types.** Convert `Color`,
`PieceType` and `Piece` (and the `MoveFlag` values where clean) to fixed-width scoped
enums, preserving every numeric value, `ALL_PIECES`/`NO_*` sentinels and all static
asserts; verify no narrowing and the size/alignment contract. M1 board work resumes at
**T005 cp2** afterward.

**T005 cp2 (corrected, next M1 task):** own-king safety → legal move generation. From
the cp1 pseudo-legal set, drop every move that leaves the own king attacked (a move
attacks the king iff it reaches the king square from its own destination); when in check,
restrict to check-escapes (king moves, captures of the checking piece, block moves).
This is a **pure correctness filter** — SEE-based *ordering/pruning* is a later **search
strength** feature (M5), not part of legal-move generation. Expose the `mg.boundary`
cases as legal. No castling yet (cp3); the perft harness (cp4) is the M1 exit.
