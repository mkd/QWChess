# QwenChess Plan

Milestone checklist with task IDs. Detailed steps only for near-term work; the
rest is an ordered backlog with objective/dependencies/exit-gate. Work is serial
(one reviewable change per task, usually one module + a few files + focused tests).

Milestones: **M0** scaffold · **M1** board correctness · **M2** minimal UCI+search ·
**M3** exact scalar NNUE · **M4** incremental NNUE · **M5** measured strength ·
**M6** SIMD/platform · **M7** SMP/advanced.

---

## M0 — source/environment lock + scaffold

### T001 — project scaffold + reference records  ✅ DONE (2026-09-18)
Establish the project, persist the corrected architecture + reference records,
build a minimal tested C17 scaffold. No chess logic.
- **Changed:** git repo; `Makefile` (GCC default/Clang opt; debug/release/sanitize;
  separate `build/<config>/`; `test`/`test-sanitize`/`net`/`test-net`);
  `src/core/types.h`; `src/platform/clock.{h,c}`; `src/engine/main.c` (self-ID
  scaffold); `tests/scaffold_test.c`; `tools/net_fetch.sh`/`net_test.sh`; root
  `AGENTS.md`/`README.md`/`.gitignore`/`COPYING`/`PROVENANCE.md`; `docs/*`.
- **Exit gate:** `make`/`debug`/`sanitize` build; `make test` + `make test-sanitize`
  pass (ASan/UBSan); `make net` fetches+verifies the net; `make test-net` controls
  pass; one commit.
- **Detail in STATE.md** (completed).

---

## M1 — board correctness / perft

### T002 — core primitives (board foundations)  ✅ DONE (2026-09-20, this commit)
- **Objective:** bitboards, move encode/decode (extend `types.h`), Zobrist hashing,
  and **portable** slider attack generation + a slow independent reference.
  **Magic tables/PEXT are explicitly out of scope here** (M6, from measurements).
- **Deps:** T001. **Files:** `src/core/{square.h,bitboard.h,attacks.* ,zobrist.*}`
  (+ C17-safe `types.h`); `tests/{sq_bb,move,attacks,zobrist}_test.c` (+ `test_util.h`);
  `tools/zobrist_gen.py`; `Makefile` (shared `-std=c17`, strict `check-c17`, per-test bins).
- **Accept (met):** bitboard ops (square 266 / bitboard 61); move round-trip (8197);
  every square/piece attack set vs an independent ray-march reference
  (1,128,714 incl. exhaustive rook+bishop); **Zobrist** — independent generator-derived
  key constants + XOR-delta vs a full recompute over 17 fixtures incl. reverse +
  state restore (187). `make check-c17` (9 files) + `make test` + `make test-sanitize`
  pass; `make clean` removes only `build/`.
 - **Still open in M1:** T004 checkpoints 4–5 (promotion / null make-unmake) →
   T005 (movegen + perft). T003 is complete; **T004 checkpoints 1 (ordinary-move),
   2 (en-passant capture) and 3 (orthodox castling) are done** and verify explicit
   make/unmake fixtures; seeded legal sequences land in T005 once movegen exists.

### T003 — Position + FEN + logical-state invariants
- **Status:** implemented, tested, and **committed** (source-audit corrections
  applied + independent Debian verification; see STATE.md for evidence and limits).
  - **Objective:** the one `Position` (mailbox + derived `occ`/`byPiece` + side +
    castling + recorded ep (canonical *for the key* via `pos_canon_ep_file`) + wide
    rule50 + hash); FEN
  parse/set; invariants (derived bitboards == recompute; hash reproducibility).
- **Deps:** T002. **Files:** `src/position/{position,fen}.*`.
- **Accept:** FEN set/parse round-trip (startpos + specials); invariants hold; a
  from-scratch hash matches the stored one. StateInfo and incremental move updates
  belong to T004. FEN draw counters are bounded by storage, not draw thresholds.

### T004 — reversible move application (make/unmake) + MoveDelta/StateInfo
 - **Status:** **checkpoints 1+2+3 done** — cp1 (ordinary moves) committed
   (`00a579c`); cp2 (en-passant capture) and cp3 (orthodox castling) verified in
   the working tree, uncommitted: `pos_make_move` / `pos_unmake_move` +
   `StateInfo`/`MoveDelta` now cover quiet / ordinary capture / pawn single+double
   push / ordinary pawn capture / EP capture (3-edit delta) / orthodox castling
   (4-edit delta; rights/ep/counters/key exact on unmake); promotion/underpromotion
   and null moves remain reserved in the delta and are rejected cleanly
   (checkpoints 4–5). See STATE.md.
- **Objective:** `make`/`unmake` restoring **all** logical state + hash exactly;
  chess-level `MoveDelta`/`StateInfo` (no NNUE types); recorded ep retained and only
  its key component canonicalized; null
  moves add no fictitious repetition; wide counters validated; repetition history
  independent of the search stack.
- **Deps:** T003. **Files:** `src/position/state.{h,c}` and position interfaces.
  Use `pos_make_move` / `pos_unmake_move`; core `make_move` already encodes a Move.
- **Accept:** hand-built quiet/pawn/capture/EP/castle/promotion/underpromotion/null
  fixtures restore every logical field, derived cache and freshly recomputed key.
  Halfmove resets on every pawn move or capture. Every double push records raw EP;
  undo restores it exactly. Guard wide-counter increments; never wrap. ASan/UBSan
  clean. Seeded legal chains require movegen and are deferred to T005.

### T005 — legal move generation + special cases + perft/differential
- **Objective:** pseudo/legal gen, captures, evasions; en-passant, castling,
  promotion/underpromotion. A perft harness **above** position+movegen (board
  core does not depend on movegen). Differential legal-move check vs an
  independent trusted generator.
- **Deps:** T004. **Files:** `src/movegen/*`; `tests/perft/*`; `tools/perft_check.*`.
- **Accept (M1 exit):** perft ≤5 on startpos (4,865,609) + ≥3 special-move FENs
  == trusted counts (fixture provenance recorded); differential movegen matches;
  make/unmake stays exact under the generator.

---

## M2 — minimal UCI + search (uses the labelled temp Material eval; decoupled from NNUE)

### T006 — labelled temp Material eval + responsive UCI protocol
- **Deps:** T005. **Files:** `src/search/material.{h,c}`; `src/engine/{uci,options,output}.*`; `src/main.c`.
- **Objective:** a **clearly labelled** temporary material evaluation (search
  plumbing only — never a NNUE fallback); UCI init/options/position/go(depth,
  movetime, infinite)/stop/quit/newgame; **a command-reader thread independent of
  the single search worker + atomic cancellation**; serialized stdout. Advertise
  only implemented options.
- **Accept:** UCI smoke test; prompt `stop` is honored; correct mate/stalemate/
  50-move handling; bounded buffers; validated input.

### T007 — iterative deepening negamax + quiescence + TT + time management
- **Deps:** T006. **Files:** `src/search/{search,tt,timeman}.*`.
- **Objective:** ID alpha-beta, check-evading quiescence, basic ordering, TT,
  soft/hard time + overhead; a legal fallback + the last completed iteration are
  kept when interrupted. PVS/aspiration are **later, isolated** steps.
- **Accept (M2 exit):** reproducible bench; plays a legal self-game; correct
  mate/draw/no-legal-move; aborted search returns a legal move + last finished
  iter; ASan/UBSan clean.

---

## M3 — exact scalar NNUE

- **T008 — strict loader + diagnostics.** Deps: T002. `src/nnue/{nnue_network,nnue_types}.*`.
  Reject missing/truncated/corrupt/incompatible nets with useful diagnostics;
  never silently substitute another evaluator.
- **T009 — scalar feature extraction (full).** Deps: T003, T008 + resolve
  `NNUE_COMPATIBILITY.md` §10 items 3–5. `src/nnue/nnue_features.*`.
- **T010 — instrumented SF19 oracle + exact scalar parity.** Deps: T009 + isolated
  reference checkout (see PROVENANCE). Byte-compare integer outputs.
- **Exit (M3):** QwenChess scalar eval == oracle (integer-exact) on all test
  positions; `nnue verify` passes; **scalar full-refresh only** (no incremental yet).

## M4 — incremental NNUE
- **T011 — accumulators + per-worker Finny + incremental update.** Deps: T010 +
  resolve §10 items 1,2,6. Caches are per-worker mutable (see ARCHITECTURE).
- **Exit (M4):** incremental == full-refresh after every move/unmove over seeded
  sequences (captures/promo/ep/castle/king/threat-ray/pawn-pair/bucket/orient/
  null/FEN-reset/undo).

## M5 — measured search strength (one feature at a time, each with evidence)
SEE ordering/pruning → histories → LMR(+re-search) → null-move (zugzwang guard) →
futility/razoring → extensions → correction history. Then paired cutechess matches
vs the last QwenChess baseline (equal resources, colors swapped, fixed seeds, full
PGN). Pre-declared stats; a match runner **must be installed before this begins**.

## M6 — SIMD / platform tuning
AVX2 kernels (exact equivalence to scalar; legal CPU-feature select; keep scalar
path) → magic/PEXT attacks (from measurements on this CPU) → LTO/PGO, cache layout.

## M7 — SMP + advanced
C11-atomics TT + worker pool (keep a deterministic single-worker debug mode) →
Chess960 → pondering → MultiPV → Syzygy. Each with explicit contracts.

---

## Ordered backlog (IDs reserved)
`T008..T010` (M3) → `T011` (M4) → `T012..` (M5 strength) → `T…` (M6) → `T…` (M7).
Reordering note: M2 search runs on the **labelled temp eval**, so search and NNUE
progress independently and merge at M5; everything else is in the stated order.
