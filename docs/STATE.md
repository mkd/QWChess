# QwenChess — State

**Milestone:** M1 (board correctness) — **T003 checkpoint 2 (FEN load/emit) COMPLETE**
this session (uncommitted). T003 remains open: cp3 make/unmake. M1 stays open
(T003–T005: Position/FEN, make/unmake, movegen+perft).
**Last verified commit:** `4644509` (T002 core primitives). Git identity `mkd`/`claudiomkd@gmail.com`.

## Completed this session (T003 cp2: FEN load/emit)
- **`src/position/fen.{h,c}`** (new): strict six-field `fen_load` (scratch+commit — a
  failure leaves the caller's Position **untouched** and sets `*why`) and `fen_emit`
  (returns 1/0; too-small buffer leaves it unchanged). Rejects: field count, spacing,
  non-square placement, bad side/castling, structurally-invalid EP, non-canonical or
  out-of-range counters, and every `pos_validate` board violation.
- **Refined EP contract (explicit):** the loader **retains the recorded EP square**
  (even when uncapturable); only the **key** canonicalizes — `pos_canon_ep_file` is the
  captor's file **iff** at least one ep capture is king-safe, else −1 (excluded from the
  key). This is the SF19-compatible rule for the *key*; the board keeps the raw record.
- **`tests/fen_test.c`** (new) + **Makefile** wiring `fen_test` (links `fen.o`, `zobrist.o`).

## Verified findings on the supplied EP fixtures (engine-probe, not labels)
Of the 8 "valid-but-uncapturable" FENs the task gave, the engine shows: **5 capturable**
(#1,2,4,5,8 — a king-safe captor), **1 genuinely uncapturable** (#3), **2 not valid
positions** (#6 missing white king, #7 ep target occupied by the black king). The test
asserts the *verified* behavior, plus clean cases + black-to-move reflections; keys are
recomputed independently via `zobrist_compute`. **The fixtures' labels need revising.**

## Latest checks (all run this session)
- `make check-c17` → **13 files OK**, no non-C17 constructs.
- `make test` (release) → all 7 binaries pass (fen: roundtrip 15, reject 63, ep 63,
  emit.bounds 7 = **148 checks, 0 fail**).
- `make test-sanitize` → all **PASS** under ASan/UBSan, no errors/leaks.

## Blockers / open items
- T003 cp3 (make/unmake, `StateInfo`/undo) remains. `reference/` Stockfish checkout not
  made (M3 oracle); match runner not installed (M5). Re-verify the EP fixtures above.

## Exactly one next task
**T003 cp3 — make/unmake** (`src/position/state.{h,c}` + `make_move`/`unmake_move`):
one board state, reversible undo stack; restores **all** logical state + key exactly;
covers captures, promotions, en passant, castling, king moves, null moves, counters;
seeded round-trip. Deps: T003 cp1+cp2 (done).
