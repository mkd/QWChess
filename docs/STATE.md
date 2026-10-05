# QwenChess — State

**Milestone:** M1 (board correctness). **T004 checkpoint 3 (orthodox castling
make/unmake) complete** and verified. M1 stays open: T004 checkpoints 4–5
(promotion/underpromotion, null) then T005 (movegen + perft) for the M1 exit.
**Baseline (last verified commit):** `00a579c` (T004 checkpoint 1; parent
`fe20ead`, T003). The checkpoint-2 (en passant) and checkpoint-3 (castling)
changes are both in the working tree, **uncommitted** (intentional — left for
review). Git identity `mkd`/`claudiomkd@gmail.com`.

## T004 checkpoint 3 — what landed
- `src/position/state.{h,c}`: `pos_make_move` / `pos_unmake_move` now support
  orthodox CASTLING. A dedicated validator (`castle_supported`) routes
  MV_CASTLE-flagged moves and requires, **before any write**: the flag exactly
  `MV_CASTLE` (a mixed `MV_EP|MV_CASTLE` / `MV_PROMO|MV_CASTLE` or a non-zero
  promotion payload is rejected on that bit); the mover's side; the origin on the
  home rank; the origin a friendly king on the e-file (the destination must be the
  h-file or a-file rook origin, so any other destination is non-orthodox); a
  friendly home rook on that destination; the applicable right set in `cr`; the
  path squares empty (f/g kingside, b/c/d queenside); and a halfmove clock that
  fits its u16 storage (incrementing at 65535 overflows, so the make is refused).
  The apply path records the reserved **4-edit** delta (king origin / rook origin /
  king dest / rook dest) and reuses the apply/unmake edit loop, so 2-, 3- and
  4-edit moves are handled alike; it clears BOTH of the moving side's rights
  (`~(WK|WQ)` / `~(BK|BQ)`) while preserving the opponent's, clears the recorded EP,
  increments the halfmove clock (Black also bumps the fullmove at 65534), updates
  the moving king's cache and rebuilds the key from the canonical en-passant file,
  so every logical field and the key come back exact on unmake. Castling is
  STRUCTURAL, not legal: it never consults king-safety (FIDE 3.8.2) — that is
  movegen's job in T005. Promotions/underpromotions and null moves remain rejected
  cleanly (checkpoints 4–5).
- `tests/castling_makeunmake_test.c` (new, wired into the Makefile): the four
  orthodox castles verified end-to-end (exact board, 4-edit delta, king caches,
  rebuild-equal key + validate-safe, exact resulting FEN, exact LIFO unmake); a
  nested W-then-B sequence with round-tripping FENs; all 8 named and all 16
  castling-rights states (opponent rights preserved; key == rebuild; full restore);
  a 31-case rejection suite (missing right, rook absent, king moved, opponent to
  move, enemy king on the destination, all 10 blocked path squares, non-orthodox
  spellings, king-to-king, from==to, mixed flags, promo payload) each asserting
  `pos` and an initialized `StateInfo` stay byte-for-byte unchanged; the five u16
  counter-boundary fixtures; and three EP-clearing fixtures. **39 sections,
  282 checks, all pass.** Sizes measured: `Position`=240, `StateInfo`=40, `MoveDelta`=20 bytes,
  `MAX_EDIT`=4 (a castle uses all four).
- `tests/makeunmake_test.c`: the malformed-move suite's `MV_CASTLE` case (the
  non-orthodox e1g1 king-to-king spelling) is now rejected by the dedicated
  `castle_supported` validator on the destination rather than as "a later
  checkpoint". 167 checks, unchanged.
- `Makefile`: `CASTLING_TEST_OBJ` / `CASTLING_TEST_BIN` + `TEST_BINS` / `DEPS` /
  link rule for the new suite. The checkpoint-2 EP suite (`ep_makeunmake_test.c`,
   38 sections, 507 checks) is unchanged and still green.

## Checks (Debian, GCC 16.2.0, this checkpoint — all re-verified today)
- `make check-c17` → 18 files, no non-C17 constructs (17 + the new castling test).
- `make test` (release) → all 11 test binaries pass, 0 fail (exit 0); the
  castling suite green (186 checks), `makeunmake` unchanged at 167, EP suite 507.
- `make test-sanitize` → all 11 binaries pass under ASan + UBSan; 0 fail, no
  sanitizer errors or leaks (exit 0).
- `make clean && make release` / `make debug` / `make sanitize` → all three
  configs build the engine from scratch, clean, no warnings under the full `WARN`
  set; the release engine runs (`uci`).

## Blockers / open
- `reference/` Stockfish 19 checkout (M3 oracle) not made; the match runner (M5)
  is not installed. Neither blocks T004. The prior T003 source-audit EP/halfmove
  findings are preserved in commit `fe20ead`.
- The checkpoint-2 (EP) and checkpoint-3 (castling) changes are uncommitted in the
  working tree (as planned); commit when reviewed.

## Exactly one next task
**T004 checkpoint 4 — promotion / underpromotion make/unmake.** Extend
`pos_make_move` / `pos_unmake_move` with the reserved 2-edit delta plus the
promotion piece type: record the original pawn at the origin, capture any enemy
piece on the destination, apply pawn→promoted piece, and restore the pawn exactly
on unmake (covers queen/knight/bishop/rook promotions and underpromotions). Then
checkpoint 5 (the null move: no board edit; side/cr/ep/counters bookkeeping),
after which T005 (movegen + perft) closes the M1 board-correctness milestone.
**Dep: checkpoint 3 (done).**
