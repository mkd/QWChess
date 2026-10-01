# QwenChess — State

**Milestone:** M1 (board correctness). **T004 checkpoint 1 (reversible make/unmake,
ordinary moves) complete** and committed. M1 stays open: T004 checkpoints 2–5
(en-passant capture, castling, promotion/underpromotion, null) then T005 (movegen +
perft) for the M1 exit. **Baseline (last verified):** the T004 checkpoint-1 commit
(parent `fe20ead`, T003). Git identity `mkd`/`claudiomkd@gmail.com`.

## T004 checkpoint 1 — what landed
- `src/position/state.{h,c}`: the `StateInfo`/`MoveDelta` undo record (per-square
  before/after, `MAX_EDIT=4`; `prev_key`/`side`/`cr`/`ep` + full-width u16 halfmove /
  fullmove) and `pos_make_move` / `pos_unmake_move`. A move is reversed by a bounded
  piece delta + recorded scalars — **no whole-Position snapshot, no heap**. The derived
  caches (`byPiece`/`byColor`/`occ`/kings) and the key update incrementally and equal a
  from-scratch rebuild. `position.h` includes `state.h`; `state.h` forward-declares
  `struct Position` (not the other way) to avoid an include cycle.
- Supported: quiet moves, ordinary captures, pawn single/double push, ordinary pawn
  captures. **Castling / promotion / en-passant capture / null moves are rejected
  cleanly** (reserved in the delta; later checkpoints). A double push records the passed
  square (FEN-preserving); every other move clears the recorded EP. Every input is
  validated **before** any write, so a rejected move leaves Position + StateInfo
  byte-for-byte unchanged. u16 counter overflow is rejected, never wrapped.
- `tests/makeunmake_test.c` (new, wired into the Makefile test runner): the 1.e4 1…c5
  2.Nf3 sequence + exact final FEN + LIFO unmake; the a1a8 castling-rights sequence; a
  double push under a legal / pinned / no-captor captor; u16 counter overflow bounds; and
  a malformed-move suite (bad encoding, castling/promo/EP/null flags, wrong-side,
  friendly/king destinations, non-attack knight, **blocked queen/rook/bishop**, off-rank
  double push). **167 checks, 0 fail.**

## Checks (Debian, GCC 16.2.0, this checkpoint)
- `make test` (release) → all 9 test binaries pass, 0 fail.
- `make test-sanitize` → all 9 pass under ASan + UBSan; no errors or leaks.
- `make check-c17` → 16 files, no non-C17 constructs. `make` builds the engine; the
  release scaffold smoke runs. No compiler warnings under the full `WARN` set.

## Blockers / open
- `reference/` Stockfish 19 checkout (M3 oracle) not made; the match runner (M5) is not
  installed. Neither blocks T004. The prior T003 source-audit EP/halfmove findings are
  preserved in commit `fe20ead`.

## Exactly one next task
**T004 checkpoint 2 — en-passant capture make/unmake.** Extend `pos_make_move` /
`pos_unmake_move` with the reserved 3-edit delta (captor origin / target landing /
captured pawn): remove the captured pawn, land the captor, clear the recorded EP, and
keep the recorded-EP/key canonicalization exact through the round trip. Then castling
(4-edit), promotion/underpromotion (2-edit + promo type), and the null move (no board
edit). **Dep: checkpoint 1 (done).**
