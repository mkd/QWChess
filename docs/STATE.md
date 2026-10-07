# QwenChess — State

**Milestone:** M1 (board correctness). **T004 checkpoint 4 (promotion /
underpromotion make-unmake) complete** and verified. Checkpoints 1–4 are done:
cp1 (ordinary moves) committed `00a579c`; cp2 (en-passant), cp3 (castling) and
cp4 (promotion) implementations landed in `e71c4b7`, and their three test suites
are committed by this reconcile. M1 stays open: **T004 checkpoint 5 (the null
move)** then T005 (movegen + perft) for the exit.
**Baseline (last verified commit):** `e71c4b7` (Implement promotions make and
unmake; parent `00a579c` cp1). The committed Makefile already referenced the three
make/unmake suites; this reconcile commits them so a clean checkout builds the full
`make test`. Git identity `mkd`/`claudiomkd@gmail.com`.

## What this reconcile landed
- `tests/promotion_makeunmake_test.c` (committed): 10 groups / 1305 checks —
  straight + diagonal + all four types on every file/color; capture-promo onto every
  enemy piece type (a-/h-file edges, king-capture rejected); a1/h1/a8/h8
  castling-rights transitions; the rejection suite (geometry, payload, mover, flags,
  reserved bits, file wrap); the canonical a7a8=Q → a8a7 undo; null stays rejected;
  a promo clears a recorded (capturable **and** uncapturable) EP target + its key
  file; nested promotions (capture of the promoted piece; two in a row); a
  structurally applied king-unsafe "pinned" promo + its mirror; wide-counter
  boundaries (hm 65535 → 0; black fm 65534 → 65535; black 65535 rejected; white
  65535 accepted).
- `tests/ep_makeunmake_test.c` + `tests/castling_makeunmake_test.c` (committed):
  the cp2 / cp3 suites the Makefile already referenced.
- Docs reconciled to the implementation: `ARCHITECTURE.md` move layout
  (`flags:3 + reserved:1`, not `flags:4`), `PLAN.md` T004 status, this file.

## Checks (Debian, GCC 16.2.0 — re-verified today)
- `make check-c17` → 19 files, no non-C17 constructs.
- `make test` (release) → all 12 binaries pass, 0 fail (exit 0).
- `make test-sanitize` → all 12 pass under ASan + UBSan, no errors/leaks (exit 0).
- `make clean && make release` / `debug` / `sanitize` → all three build the engine
  clean, no warnings under the full `WARN` set; the release engine self-IDs (`uci`).

## Blockers / open
- `reference/` SF19 checkout (M3 oracle) not made; the match runner (M5) is not
  installed. Neither blocks T004/T005.

## Exactly one next task
**T004 checkpoint 5 — the null move.** Extend `pos_make_move` / `pos_unmake_move`
with the null move (move value 0): no board edit; record + restore side, castling,
EP and the counters, and keep the key exact, while a null move adds **no fictitious
game repetition**. Then T005 (movegen + perft) closes M1. **Dep: checkpoint 4 (done).**
