# QwenChess — State

**Milestone:** M1 (board correctness) — **T002 COMPLETE** (all four test groups:
scaffold/square+bitboard, move, attacks, **Zobrist**). M1 stays open (T003–T005:
Position/FEN, make/unmake, movegen+perft).
**Last verified commit:** the T002 core-primitives commit (this change), on top of
`552d4d3` (T01 scaffold). Git identity `mkd`/`claudiomkd@gmail.com`.

## Completed this session (T002 checkpoint 4: independent Zobrist verification)
- **`tools/zobrist_gen.py`** (new, retained): standalone SplitMix64 PRNG with the
  documented seed `0x9E3779B97F4A7C15` + draw order (1024 piece-square + 1 side +
  16 castling + 8 ep-file). Re-run reproduces the key tables byte-identically;
  cross-checked equivalent against a throwaway C replica of the production generator.
- **`tests/zobrist_test.c`** (new; links production `zobrist.{h,c}`; always-on):
  (A) independent key constants from the generator must equal the production tables
  (both colors; start/end of each family; every castling/ep entry); out-of-range
  guards yield identity 0; idempotent deterministic reinit. (B) XOR-delta vs an
  **independent full recompute** (scans all 64 squares + state keys; never derives
  from the delta) over **17** explicit before/after fixtures — quiet (both colors),
  capture, promotions Q/R/B/N, promotion-capture, en-passant (cap removed at the
  ACTUAL square, not `to`), castling (rook leg), king-move/rook-move/rook-capture
  rights-loss, ep create/expire/replace, and a capture+ep+side combination. Each
  checks forward + reverse key, grounds the reference against `zobrist_compute`, and
  verifies the restored state + its recomputed key.
- **`Makefile`**: added the `zobrist_test` binary (additive; prior wiring untouched).
  **`types.h`**: clarified the Move-encoding comment (3 flag bits + reserved bit 15);
  no behavior change. Rule-50 / repetition history remain deliberately outside the key.

## Latest checks (all run this session)
- `make check-c17` → **9 files OK, no non-C17 constructs** (zobrist_test.c added).
- `make test` (release) → all pass, 0 fail: scaffold; sq_bb 266+61; move
  8197+384+19+8+16; attacks 1,128,714; **zobrist 63+5+119 = 187**.
- `make test-sanitize` → all **PASS** under ASan/UBSan, no sanitizer errors.
- `make` / `make debug` / `make sanitize` → engine builds, no warnings.
- `make clean` → removes only `build/`; no stray `.o`/`.d`/bin remain outside it.

## Blockers / open items
- `reference/` Stockfish checkout not made (M3 oracle). Match runner
  (cutechess/fastchess) not installed (M5). No blockers for T003.
- Zobrist key updates verified **in isolation** (explicit states). Re-verify through
  seeded legal make/unmake sequences (null moves + in-context EP canonicalization)
  once T004/T005 land make/unmake + movegen.

## Exactly one next task
**T003 — Position + FEN + logical-state invariants** (`src/position/{position,fen}.*`):
the one `Position` (mailbox + derived `occ`/`byPiece` + side + castling +
canonicalized ep + wide rule50 + hash + `StateInfo` link); FEN set/parse; invariants
(derived bitboards == recompute; from-scratch hash == incremental). Deps: T002 (done).
