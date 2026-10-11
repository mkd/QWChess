# QwenChess — State

**Milestone:** M1 (board correctness). **T004 complete and verified.** This
checkpoint verifies the two latent-defect fixes from the last handoff (a non-pawn
move recording a fictitious EP target; `pos_repetition_count` crossing a null into
an earlier real position), the completed `rep.storage` live-chain path, and
reconciles the sanitizer evidence (see Checks). M1 stays open: **T005 (movegen +
perft)** is the last M1 task.
**Baseline (last verified commit):** `74033a6` (T004 cp5: null move + repetition
history). Git identity `mkd`/`claudiomkd@gmail.com`.

## Baseline (`74033a6`): T004 cp5 — null move + repetition history
Already committed; listed for context (this checkpoint builds on it).
- `pos_make_null_move` — a **distinct** null/pass entry point (the ordinary
  `pos_make_move` still rejects the move value 0, the no-move sentinel). Zero
  board edits; flip the side; clear the recorded EP; **preserve both counters**
  (halfmove + fullmove) for either color; exact incremental key (toggle the side,
  drop the old canonical-EP contribution). Rejected (both outputs unchanged) when
  the side to move is in check — a pass cannot address a check. `pos_unmake_move`
  handles the null via the shared zero-edit + prev_* path.
- **Repetition history:** `StateInfo.prev` links each record (real or null) to its
  predecessor; `Position.history` points at the latest record (NULL on a fresh
  setup). Caller-owned, stable addresses, **not capped at MAX_PLY**. `Position`
  holds only a non-owning pointer; `pos_rebuild` leaves it untouched, a successful
  FEN/reset setup NULLs it, a failed load preserves it.
- **`pos_repetition_count`** — the production occurrence query (read-only, no
  allocation). A fresh setup → 1; a null-produced node → 0 **immediately** (the
  defect-2 fix: a null's key can still equal an earlier real position, so it must
  not be counted across the null); the ancestor walk (real nodes) never crosses a
  null transition. Occurrence only, not draw adjudication.
- Two new test suites: `tests/null_makeunmake_test.c` (4 groups, 596 checks) and
  `tests/repetition_test.c` (5 groups, now incl. the defect-2 regression) — both
  wired into `make test`.
  Note on `basic_cycle`: after the second cycle, undoing the two closing moves lands
  on the position after move 6, whose key equals the position after move 2 (both are
  startpos + Nf3 + Nf6, white to move), so the reachable history holds it twice and
  the correct count is **2**; the test asserts 2 and then undoes all the way back to
  1 at the root (the task asked for the prior count to be restored, not a specific
  intermediate value).
- Docs reconciled to the implementation: `ARCHITECTURE.md` reversible-move contract
  (the `SquareEdit`/`MoveDelta`/`StateInfo` structs, the null + history, the
  ownership table), `PLAN.md` T004 status, this file.

## This checkpoint commits (verified + committed)
- **EP record (cp1/`pos_make_move`).** `is_double_push` is geometry-only, so a
  rook/queen on the pawn starting rank moving two ranks forward recorded a
  fictitious EP target (`7k/.../3R4/K7 w` + d2d4 → ep d3; `pos_validate` then
  failed, the key stayed correct). Fix: the record is written only when the mover
  is a pawn. Regression: `makeunmake.ep_nonpawn` (rook/queen × both colors × quiet
  + capture).
- **`pos_repetition_count` (cp5).** A null that restored the board to a previously
  seen arrangement counted 1 (the walk crossed the null). Fix: return 0
  immediately for a null head; the parent-transition check is kept for real
   descendants. Regression: `rep.null_match_boundary` (exact sequence + mirror);
   also completed the untested live-head portion of `rep.storage` (a distinct
   search array attached to the still-live 260-ply head: 66 → 67 → back to 66).
- **Docs/evidence.** `state.h` null/repetition comments reconciled; the
  intermediate repetition count documented as the correct 2 (the false "prompt
  demanded 1" claim removed); deferred C++20 + platform-tuning roadmap entries
  added to `PLAN.md`/`ARCHITECTURE.md` and the C17-phase note to `AGENTS.md`;
  this file reconciles the sanitizer evidence below.

## Checks (this machine, re-verified)
- `make check-c17` → **21 files**, no non-C17 constructs. **(this machine: exit 0)**
- `make release debug sanitize` → all three engine binaries build, no warnings. **(exit 0)**
- `make test` (release) → all **14** binaries pass, 0 fail. **(this machine: exit 0; 143 PASS lines)**
- `make CONFIG=debug test-run` → all **14** pass, 0 fail. **(exit 0)**
- `git diff --check` → no whitespace errors. **(exit 0)**
- **`make test-sanitize` — this machine: the ordinary target does NOT run green.**
  All 14 instrumented binaries build, but each aborts at ASan startup with
  `ASan runtime does not come first in initial library list` (exit 2). Root cause
  is environmental, not a build defect: `/etc/ld.so.preload` injects a setuid,
  non-dpkg library `/usr/lib/libenable-3fg-drag.so` that the loader places **before**
  `libasan`, so ASan's link-order check trips. The build is correct (ASan+UBSan reach
  compile **and** link; `libasan.so.8`/`libubsan.so.1` are proper `DT_NEEDED`).
  **Alternate launch (documented):** run each of the 14 from the Makefile list with
  `LD_PRELOAD=$(gcc -print-file-name=libasan.so)` scoped to the test processes →
  **14/14 pass** (1,142,994 checks, 0 failed, 0 runtime diagnostics; ASan+LSan
  confirmed active via the init banner). No Makefile change: the portable build
  defaults are correct and must not be altered for one host's injected preload.
- **Historical (prior review/environment, not this machine):** the last handoff
  reported `make test-sanitize` all-14 green (exit 0); that was a different
  environment without the injected preload and is retained here as history only.
- New this checkpoint: `makeunmake.ep_nonpawn` 104 checks (8 cases), `rep.null_match_boundary` 38 checks, `rep.storage` 274 → 286.

## Blockers / open
- **Sanitizer normal target is runner-limited on this host** (not a code/build
  defect): the injected setuid `/etc/ld.so.preload` library breaks the ordinary
  `make test-sanitize`; use the documented `LD_PRELOAD` alternate launch. Does not
  block T005 (release/debug + C17 are fully green).
- `reference/` SF19 checkout (M3 oracle) not made; the match runner (M5) is not
  installed. Neither blocks T005.

## Exactly one next task
**T005 — legal move generation + special cases + perft/differential** (closes M1).
**Dep: T004 (done).** A perft harness above position+movegen; differential
legal-move check vs an independent generator; make/unmake stays exact under the
generator. Seeded legal make/unmake sequences (deferred from T004) land here.
