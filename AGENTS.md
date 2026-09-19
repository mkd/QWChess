# QwenChess — agent rules (read this first)

C17 UCI chess engine. New codebase; reuses Stockfish 19 (pinned commit, see
`PROVENANCE.md`) algorithms/NNUE with attribution. GPL-3.0-or-later.

## On session start, read ONLY:
1. This file.
2. `docs/STATE.md` (current milestone, last verified commit, exactly one next task).
3. `docs/PLAN.md` — only the current task's section.
4. Only the contract/source files that task names.
Do **not** reload the whole repo or all upstream NNUE source; read on demand.

## Build / test (Debian; GCC default, `CC=clang` optional; no CMake)
```
make | make debug | make sanitize
make test | make test-sanitize
make net | make test-net        # net fetch/verify (offline build never needs this)
```
Separate `build/<config>/` trees — never mix objects across configs.

## Hard rules
- All shipped logic is C17. No C++, no C++ runtime, no external eval process,
  no LLM/network at play time. Upstream SF19 C++ is a *test oracle/opponent* only.
- **One board representation = `Position`** (authoritative chess state). NNUE is a
  *consumer* of Position and owns its feature indices + pending updates; it never
  owns a board. **Position/StateInfo/MoveDelta must contain no NNUE-specific types.**
- **Ownership:** only *network weights* and *immutable lookup tables* are shared
  read-only. *Finny/accumulator refresh caches are mutable, per-worker* state; reset
  them on any network change. Derived piece snapshots in a cache are allowed.
- One search limit `MAX_PLY=256` (derived from QwenChess, not SF's 246); one score
  type `int32_t Value`. Root-relative ply ≠ game history ≠ FEN fullmove; no `u8`
  rule50 (use wide, validated counters).
- No heap allocation in the search/eval hot path; preallocate per worker.
- make/unmake restore ALL logical state + hash exactly; canonicalize en-passant;
  null moves create no fictitious repetition.
- NNUE integer pipeline follows SF19 exactly (types/order/clipping); no UB
  (signed shifts, overflow, aliasing, narrowing handled explicitly).
- The temp `Material` eval is LABELLED, for search plumbing only — never a NNUE fallback.
- **Responsive UCI is required from the first UCI milestone:** a command-reader
  thread independent of the (single) search worker + atomic cancellation. Only
  *multiple* search workers are deferred.
- Keep protocol output serialized to stdout; other diagnostics elsewhere.

## Evidence discipline
- Correctness ≠ speed ≠ strength. Never claim a test passed unless you ran it and it passed.
- Perf-only changes must preserve behavior; search changes need match evidence.

## Every task handoff
Report: changed behavior, commands run, observed results, unrun/blocked checks,
remaining risks, next task. Update `docs/STATE.md`. One commit per task. Preserve
user edits; never rewrite git history. Stop after the assigned task.
