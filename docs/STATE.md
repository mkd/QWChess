# QwenChess — State

**Milestone:** the **CPP20 modernization workstream** (a language/refactor track, not a
functionality milestone). This checkpoint is the **CPP20-2B closeout**: the project
namespace + frozen naming, the remaining typed constants, all fixed-size storage →
`std::array` (+ the `.data()`/typed-init boundary), the bitboard endianness/width
documentation + portable-fallback target, and the full build-matrix verification.
**M1 board work (T001–T005 cp1) is preserved** — behavior and test evidence are identical
in *chess semantics*; only types, naming, constants, storage, comments and the build
changed. The full regression suite passes in all three configurations.
**Baseline (last verified commit before this checkpoint):** `8335834` (CPP20-2A closeout).
Git identity `mkd`/`claudiomkd@gmail.com`.

## This checkpoint commits (CPP20-2B, five ordered batches)
1. **names + namespaces:** every domain type into `namespace qwc` (`Square`/`Piece`/
   `Color`/`PieceType`/`MoveFlag`/`Move`/`Value` + `Position`/`StateInfo`/`MoveDelta`/
   `SearchState`/`SearchLimits`/`SearchResult`); PascalCase types, snake_case
   functions/variables, UPPER_SNAKE constants/enumerators; the `enum`→`enum class`
   byte/value boundary re-expressed with `to_under`/`static_cast`; tests use
   `using namespace qwc` (engine keeps full qualification).
2. **constants:** numeric constants frozen as typed `constexpr` (`POS_HM_MAX` u16,
   `POS_FM_MAX` u32, `FEN_BUF_SIZE` std::size_t, `DIR_NB` int, `MAX_PLY` int,
   `MAX_PLY_STACK` size_t) — the raw `0xFFFF`/`0xFFFFFFFF`/`1<<30` spellings removed.
3. **storage:** all fixed-size *data* arrays → `std::array` (Position `mailbox`/
   `by_piece`/`by_color`; `MoveDelta.squares`/`pieces`/`occ_delta`/`king_sq_delta`;
   attack/zobrist/fen tables + the EP scratch board; the TState test struct). The
   C-string/move-buffer boundary takes `.data()`/`.size()`; byte-ops become typed
   (`fill`, `==`, value-init, value-copy) with no `sizeof T*sizeof T` / `memcmp`-equality;
   the **retained** `char out[3]`/`char dst[512]`/`const char**` **function parameters**
   stay pointers.
4. **bitboards:** `bitboard.hpp` documented as endianness-agnostic (a bit-*index*
   convention, not byte order) + a 64-bit-width `static_assert` trap; the `QWC_FORCE_
   PORTABLE_BITS` fallback gains a reproducible `make test-portable-bits` target.
5. **build matrix + verification:** `make check-c` (a new C-residue gate) added; the
   full matrix (below) verified; docs (PLAN/ARCHITECTURE/STATE/REQUIREMENTS) reconciled.

## Checks (this host, GCC 16.2.0 — **newly run this checkpoint**)
- `make check-c` → **OK** (no C-only constructs / C-compiler extensions, 23 files).
- `make check-cpp20` → **23 files**, exit 0 (no non-C++20 constructs).
- `make clean && make -j14` (release) → builds, **0 warnings/0 errors** under
  `-Wall -Wextra -Wshadow -Wconversion -Wwrite-strings -std=c++20 -O2 -march=native`.
- `make test` (release) → **15/15 suites, 186 PASS, 0 fail**, exit 0.
- `make test CONFIG=debug` → **15/15, 186 PASS, 0 fail**, exit 0.
- `make test CONFIG=sanitize` → instrumented binaries **build** 0 warnings; the **normal
  run fails on this host** (ASan "runtime does not come first… LD_PRELOAD", exit 2) —
  root cause is `/etc/ld.so.preload` → `libenable-3fg-drag.so` (a VM tool) force-loading
  into every process, which breaks ASan for *all* programs (a minimal `-fsanitize=address`
  binary fails identically). Under the **documented `LD_PRELOAD=libasan.so.8` workaround,
  all 15 suites run and pass (186, 0 fail, 0 ASan/UBSan/leak issues)** — a workaround,
  not ordinary coverage. (Carried from CPP20-2A; still applies.)
- `make test-portable-bits` → the `QWC_FORCE_PORTABLE_BITS=1` fallback path: bitboard test
  **327 checks, 0 fail**, exit 0.
- Raw C array **declarations** in `src/` (production): **0** (all data arrays are
  `std::array`; the only `[N]` are comments, `std::array` subscripts, and the retained
  `char out[3]` param). New `#include <cstdint>`: **0**. `static_assert` layout contract
  unchanged (Position 248/8, StateInfo 48/8, MoveDelta 20/4, SquareEdit 3/1) — the
  `std::array` conversion is layout-neutral.

## Blockers / open
- **Clang not installed** here; only GCC/g++ 16.2.0 verified (carried to CPP20-3/4, which
  must validate both; AArch64/RV64 reported separately, not claimed).
- **Sanitizer *run*** requires the `LD_PRELOAD` workaround on this host (above). The
  instrumented binaries build and pass cleanly via the preload.
- `.gitignore` (last touched CPP20-1) covers `build/`/`networks/`/`reference/`/cruft; the
  known **build-only gap is carried forward, not closed** (no build artifact in-tree).
- `reference/` (M3 oracle) and the match runner (M5) are not made; neither blocks 2B.

## Exactly one next task
**T005 cp2 — own-king safety (legal move generation)** (the M1 functional track resumes
here; the CPP20-2B refactor is complete). For each pseudo-legal candidate, save the
mover's color, `pos_make_move` + `pos_unmake_move`, and keep the candidate iff the mover's
king is unattacked on the resulting board (including attacks from pinned enemy pieces).
A pure correctness filter (no SEE pruning); the deliberately-unsafe `mg.boundary`
candidates must be absent. See PLAN.md T005 cp2. (CPP20-3 — interfaces/`std::span`/
output/docs — is the next *C++20* item and interleaves on the parallel track.)
