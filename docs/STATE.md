# QwenChess — State

**Milestone:** M1 (board correctness). **T005 cp1 complete and verified.** This
checkpoint (the new movegen commit) adds `src/movegen/` with a single
allocation-free entry point, `movegen_pseudo_noncastling`, generating every
non-castling pseudo-legal candidate (geometric moves/captures, all four
promotions, structural en passant) from the existing bitboards/attack tables —
**no** own-king-safety filter, **no** castling. M1 stays open: **T005 cp2
(own-king safety → legal gen)** is next.
**Baseline (last verified commit):** `a388ff4` (T004: EP-bookkeeping +
null-repetition fixes); it follows `74033a6` (T004 cp5). This checkpoint builds
on it. Git identity `mkd`/`claudiomkd@gmail.com`.

## Prior work (the T004 line this builds on)
`74033a6` (cp5) added `pos_make_null_move`, the caller-owned repetition history
(`StateInfo.prev` / `Position.history`), and `pos_repetition_count`; `a388ff4`
fixed two latent defects (a non-pawn move recording a fictitious EP target;
`pos_repetition_count` crossing a null into an earlier real position). Both are
in place and green — `Position`/`StateInfo`/`MoveDelta`, the full make/unmake set,
and 13 test suites. The movegen module is the one addition in this checkpoint.

## This checkpoint commits (T005 cp1 — non-castling pseudo-legal movegen)
- **New `src/movegen/` module** (depends on core+position only; a strict-DAG leaf
  before search/nnue; **stateless** — reads `Position`, writes to a caller buffer).
  Frozen header API `movegen_pseudo_noncastling(const Position*, Move*, size_t
  capacity, size_t *count)`: succeeds → complete count; fails (NULL pos/out, or
  result > capacity) → 0. Allocation-free, read-only over `Position` (no make/
  unmake, no rebuild, no king-safety query, no heap). Deterministic, duplicate-free
  enumeration. Uses the existing `Position` bitboards + attack tables; adds no
  board representation, no SIMD, no magic attacks, no NNUE fields.
- **Generator (`movegen.c`):** friendly pieces via `byColor` (knight/king masks;
  sliders over combined occupancy, excluding friendly occ + the opposing king; own
  king safety deliberately absent). Pawns: single/double push (file+rank bounds,
  no wrap), diagonal captures (enemy non-king), all four promotions on the final
  rank. En passant (structural): recorded target + just-pushed enemy pawn + empty
  landing + an adjacent friendly captor → `MV_EP`, zero payload; up to two captors
  → two moves; **no** `pos_ep_is_legal`/king-safety call.
- **`tests/movegen_pseudo_test.c`** (wired into `make test`): **41 groups / 2561
  checks.** 14 independent candidate sets (python-chess 1.11.2, prompt time) and
  their rank-reflected/color-swapped mirrors — complete encoded Move set
  (order-independent), no duplicates; flag/payload semantics checked separately
  (EP→`MV_EP`, promo suffix→`MV_PROMO`+N/B/R/Q, else quiet; no castling/reserved
  bits/null). Exact startpos (20). Deliberate boundary cases (king-in-check keeps
  `e1d2`/`e1e2`; pinned/mixed EP keep `e5d6`). A blocked-first-pawn-step case.
  Capacity 20/19/0 + NULL ptrs + canary. Determinism + a live history prefix.
  Counter independence (0/1 vs 65535/65535). Per-candidate `pos_make_move`/
  `pos_unmake_move` round-trips (structural success; child caches+key == cold
  `pos_rebuild`; exact restore) across EP/promotion/quiet/unsafe-child boards.
- **Docs:** `ARCHITECTURE.md` movegen module (stateless; cp1 scope) + the
  not-yet-created note; `PLAN.md` T005 split into cp1 (done) / cp2 / cp3 / cp4.

## Checks (this machine, re-verified)
- `make` (default = release) → `qwenchess` builds, no warnings. **(exit 0)**
- `make test` (release) → all **15** binaries pass, 0 fail. **(exit 0; 184 PASS
  lines; new suite = 41 groups / 2561 checks / 0 failed)**
- `make CONFIG=debug test-run` → all **15** pass, 0 fail. **(exit 0)**
- `make check-c17` → **23 files** (was 21; +`src/movegen/movegen.c` + the test),
  no non-C17 constructs. **(exit 0)**
- `git diff --check` → no whitespace errors. **(exit 0)**
- **`make test-sanitize` — this machine: the ordinary target does NOT run green**
  (all 15 build, but each aborts at ASan startup with `ASan runtime does not come
  first in initial library list`). Same environmental root cause as T004:
  `/etc/ld.so.preload` injects a setuid non-dpkg library placed before `libasan`.
  Not a build defect. **Alternate launch:** run each of the 15 with
  `LD_PRELOAD=$(gcc -print-file-name=libasan.so)` → **15/15 pass** under ASan+UBSan
  (0 runtime diagnostics). No Makefile change.

## Blockers / open
- **Sanitizer normal target is runner-limited on this host** (not a code/build
  defect): the injected setuid `/etc/ld.so.preload` library breaks the ordinary
  `make test-sanitize`; use the documented `LD_PRELOAD` alternate launch (15/15
  green). Does not block T005 cp2.
- `reference/` SF19 checkout (M3 oracle) and the match runner (M5) are not made.
  Neither blocks cp2.

## Exactly one next task
**T005 cp2 — own-king safety (legal move generation).** From the cp1 pseudo-legal
set, drop every move that leaves the own king attacked (a move attacks the king iff
it reaches the king square from its own destination); if in check, restrict to king
moves + check-captures (exclude quiet moves); add SEE-based pruning (a candidate
must survive SEE; a pinned captor's EP may be pruned); keep every capture/
promotion-escape/EP-escape. Expose the `mg.boundary` cases as legal. No castling
yet (cp3), no perft harness yet (cp4 = the M1 exit).
