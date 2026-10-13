# QwenChess — agent rules (read this first)

C++20 UCI chess engine. New codebase; reuses Stockfish 19 (pinned commit, see
`PROVENANCE.md`) algorithms/NNUE with attribution. GPL-3.0-or-later.

## On session start, read ONLY:
1. This file.
2. `docs/STATE.md` (current milestone, last verified commit, exactly one next task).
3. `docs/PLAN.md` — only the current task's section.
4. Only the contract/source files that task names.
Do **not** reload the whole repo or all upstream NNUE source; read on demand.

## Build / test (Debian; g++ default, `CXX=clang++` optional; no CMake)
```
make | make debug | make sanitize
make test | make test-sanitize
make check-cpp20     # strict -std=c++20 -pedantic-errors gate over all sources
make net | make test-net        # net fetch/verify (offline build never needs this)
```
Separate `build/cpp20/<config>/` trees — never mix objects across configs.

## Hard rules
- All shipped logic is **C++20**. The C++ standard library is permitted at the
  implementation level (fixed-width types, `<bit>`, `<cstring>`, `<chrono>`, …).
  Prohibited at play time: LLM calls, remote services, and any external evaluator
  process. Upstream SF19 C++ remains a *test oracle/opponent* only. C++ does **not**
  relax the determinism, ownership, memory-layout, score, or synchronization
  contracts. Prohibit exceptions and RTTI (`dynamic_cast`/`typeinfo`).
- **C++20 source portability ≠ runtime compatibility.** It does not prove the engine
  runs on every CPU/OS; each target still needs its own build + tests (CPP20-3).
- **One board representation = `Position`** (authoritative chess state). NNUE is a
  *consumer* of Position and owns its feature indices + pending updates; it never
  owns a board. **Position/StateInfo/MoveDelta must contain no NNUE-specific types.**
  These stay plain aggregates — no user-declared constructors/destructors yet
  (no `std::array`/`span`/`string_view`; that is CPP20-3).
- **Ownership:** only *network weights* and *immutable lookup tables* are shared
  read-only. *Finny/accumulator refresh caches are mutable, per-worker* state; reset
  them on any network change. Derived piece snapshots in a cache are allowed.
- One search limit `MAX_PLY=256` (derived from QwenChess, not SF's 246); one score
  type `int32_t Value`. Root-relative ply ≠ game history ≠ FEN fullmove; no `u8`
  rule50 (use wide, validated counters).
- No heap allocation in the search/eval hot path; preallocate per worker.
- make/unmake restore ALL logical state + hash exactly; retain recorded en-passant
  targets after every double pawn push and canonicalize only their key component;
  null moves create no fictitious repetition.
- NNUE integer pipeline follows SF19 exactly (types/order/clipping); no UB
  (signed shifts, overflow, aliasing, narrowing handled explicitly).
- The temp `Material` eval is LABELLED, for search plumbing only — never a NNUE fallback.
- **Responsive UCI is required from the first UCI milestone:** a command-reader
  thread independent of the (single) search worker + atomic cancellation. Only
  *multiple* search workers are deferred.
- Keep protocol output serialized to stdout; other diagnostics elsewhere.

## C++20 conventions (this codebase)
- **Standard headers** only (`<cstdint>`,`<cstring>`,`<cstdio>`,`<cstddef>`,`<chrono>`,
  `<bit>`), not the C headers. **`std::`-qualify** standard-library functions/types.
  `nullptr` (not `NULL`); `static_assert` (not `_Static_assert`).
- `using` aliases for fixed-width types; plain `struct` declarations (no
  `typedef struct … x;`). Header helpers are `inline`; pure geometry/encoding
  helpers may be `constexpr` without changing the edge-case contract. Bitboard
  count/scan uses `<bit>`; the forced portable fallback (`QWC_FORCE_PORTABLE_BITS=1`)
  is retained for tests.
- C-style casts are allowed **only** at genuine representation boundaries
  (`u8`→`int`, sentinel→`Square`, …); otherwise `static_cast`. No narrowing.
  Preserve `NO_SQUARE`/sentinels: zero-init does not make a `Position` valid —
  `pos_rebuild`/`pos_set_startpos` must run. Test asserts stay active under `NDEBUG`.

## Evidence discipline
- Correctness ≠ speed ≠ strength. Never claim a test passed unless you ran it and it passed.
- Perf-only changes must preserve behavior; search changes need match evidence.

## Every task handoff
Report: changed behavior, commands run, observed results, unrun/blocked checks,
remaining risks, next task. Update `docs/STATE.md`. One commit per task. Preserve
user edits; never rewrite git history. Stop after the assigned task.
