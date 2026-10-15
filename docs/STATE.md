# QwenChess — State

**Milestone:** the **CPP20 modernization workstream** (a language refactor, not a
functionality milestone). This checkpoint is **CPP20-2 (scoped enums)**, now complete
and verified. All domain values are fixed-width **scoped** enums with `to_under` at the
value/byte boundary; `M1` board work (T001–T005) is preserved — behavior and test
evidence are identical, only the types changed.
**Baseline (last verified commit):** `a05f3bd` (CPP20-1: C17 → C++20 language
migration). This checkpoint builds on it. Git identity `mkd`/`claudiomkd@gmail.com`.

## This checkpoint commits (CPP20-2 — scoped enums with explicit underlying types)
- **`types.hpp`:** `PieceType`, `Piece`, `Color` become `enum class X : u8` and
  `MoveFlag` becomes `enum class MoveFlag : u32` (replacing CPP20-1's
  `using X = u8` alias + separate plain value-enum). The `*_NB` counts (`8`/`16`/`2`)
  move to `constexpr int` — a scoped enum may only name real domain values. Every
  constant keeps its exact numeric value (verified by the `static_assert`s, now
  `std::is_enum_v<…>` + `to_under(…)`). Added `to_under<E>` (the single documented
  typed→raw boundary, via `std::underlying_type_t` — C++11, not C++23
  `std::to_underlying`) and `piece_color`/`piece_type` helpers on top of it.
  **`Square` stays the one-byte `u8` alias** + `enum SquareSentinel { NO_SQUARE = 64 }`:
  squares are 0..63 (computed by `square_of`), with no per-square enumerators, so there
  is nothing to scope. `square_of` is `constexpr` over the `Square`/`u8` domain.
- **Source** (`position`, `state`, `fen`, `movegen`, `zobrist`, `attacks`): every
  value/byte/table-index/raw-bit boundary re-expressed with `to_under`/`static_cast`/
  the `(Piece)`/`(Color)`/`(u8)` representation casts; `Piece mailbox[]`, `Color`,
  `Square` struct fields and API params are typed. No chess-rule logic changed.
- **Tests:** the existing `QWC_*` harness auto-converts a scoped-enum operand
  (`qwc_as_int`/`qwc_as_u64`) so raw-value fixtures are untouched; the raw-bit
  fixtures, expected values, and per-bit move-encoding contracts are unchanged.
- **`Makefile`:** added the previously-missing `$(MAKEUNMAKE_TEST_OBJ:.o=.d)` to the
  `DEPS` line (wires the makeunmake test into dependency tracking, like every other
  test). Pre-existing uncommitted fix, included here for a coherent tree.

## Checks (this host, GCC/g++ — re-verified this session)
- `make check-cpp20` → **23 files**, no non-C++20 constructs. **(exit 0)**
- `make clean && make CONFIG=release` → **0 errors, 0 warnings**
  (`-Wconversion -Wextra -Wall -Wshadow -Wwrite-strings` → no narrowing/implicit conv).
- `make test CONFIG=release` → **15/15** binaries, **184 PASS groups, 0 fail**
  (**1,145,555 checks, 0 failed**). **(exit 0)**
- `make test CONFIG=debug` and `make test CONFIG=sanitize` → **15/15, 184 PASS, 0
  fail**; the sanitize tree runs clean (no ASan/UBSan runtime reports). **(exit 0)**
- **Size/alignment contract HELD** (measured with a throwaway probe): Position 248/8,
  StateInfo 48/8, MoveDelta 20/4, SquareEdit 3/1; `sizeof` Piece/Color/PieceType/Square
  all 1, MoveFlag 4 (not struct-stored). Unchanged vs the CPP20-1/C baseline.

## Blockers / open
- **Clang is not installed** on this host (`clang++` absent); only GCC/g++ was
  verified. The cross-compiler Clang build (opt-in `CXX=clang++`) must be checked on a
  Clang-capable host — carried to **CPP20-3**'s multi-arch validation.
- `reference/` SF19 checkout (M3 oracle) and the match runner (M5) are not made;
  neither blocks CPP20-3.

## Exactly one next task
**CPP20-3 — storage modernization + interface cleanup + multi-arch validation.**
Adopt `std::array` for the fixed-size arrays and references/pointers for output
parameters (and `constexpr`/`operator==` only where justified); replace
`std::numeric_limits`/`INT64_MAX`-style macros; keep the plain-aggregate + ownership
contracts. Validate the GCC build here and the Clang build on a Clang-capable host
(both green incl. `check-cpp20`); report AArch64/RV64 support separately — do not
claim untested architecture support. M1 board work resumes at **T005 cp2** after the
CPP20 tasks.
