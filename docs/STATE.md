# QwenChess — State

**Milestone:** M1 (board correctness). **T004 checkpoint 5 (the null move +
repetition history) complete** and verified — this closes T004 (all five
checkpoints). M1 stays open: **T005 (movegen + perft)** is the last M1 task.
**Baseline (last verified commit):** `00ce9d3` (T004 cp2-4 reconcile). Git
identity `mkd`/`claudiomkd@gmail.com`.

## What this task landed
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
  allocation). A fresh setup → 1; a null-produced node → 0; the walk never crosses
  a null transition. Occurrence only, not draw adjudication.
- Two new test suites: `tests/null_makeunmake_test.c` (4 groups, 596 checks) and
  `tests/repetition_test.c` (4 groups, 334 checks) — both wired into `make test`.
  Note: the task spec asked the second-cycle undo to "return to 1"; the correct
  value there is **2** (the position after move 6 equals the position after move 2),
  so the test asserts 2 and undoes all the way to 1 at the root.
- Docs reconciled to the implementation: `ARCHITECTURE.md` reversible-move contract
  (the `SquareEdit`/`MoveDelta`/`StateInfo` structs, the null + history, the
  ownership table), `PLAN.md` T004 status, this file.

## Checks (Debian, GCC — re-verified today, all three configs)
- `make check-c17` → **21 files**, no non-C17 constructs.
- `make test` (release) → all **14** binaries pass, 141 suites, 0 fail (exit 0).
- `make test-sanitize` → all 14 pass under ASan + UBSan, 0 runtime errors (exit 0).
- debug config `test-run` → all 14 pass, 0 fail (exit 0).
- New suites: `null` 596 checks (336/102/123/35), `rep` 334 checks (24/8/28/274).

## Blockers / open
- `reference/` SF19 checkout (M3 oracle) not made; the match runner (M5) is not
  installed. Neither blocks T005.

## Exactly one next task
**T005 — legal move generation + special cases + perft/differential** (closes M1).
**Dep: T004 (done).** A perft harness above position+movegen; differential
legal-move check vs an independent generator; make/unmake stays exact under the
generator. Seeded legal make/unmake sequences (deferred from T004) land here.
