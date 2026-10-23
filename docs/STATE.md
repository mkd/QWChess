# QwenChess — current state

**Milestone:** `T005 cp3` (done this commit) — **full** orthodox legal move generation
(`movegen_legal`): the cp2 non-castling legal moves **plus** the legal castles. M1
continues with **cp4 (perft + differential)** next — the M1 exit gate.

**Last verified commit:** `f9aab8a` (T005 cp2) is the parent; this commit is T005 cp3.

## What this checkpoint did
- **Full legal generator** (`src/movegen/movegen.{hpp,cpp}`): `movegen_legal(const
  Position&, std::span<Move>, std::size_t&)` returns the complete orthodox legal set =
  `movegen_legal_noncastling` + the legal castles. Encoding is **king-origin →
  rook-origin** (e1h1/e1a1/e8h8/e8a8; MV_CASTLE, zero promo payload), applied by
  `pos_make_move` as king→g/c + rook→f/d. A castle is legal iff the mover holds the
  right, the home king/rook are in place with an **empty path** (kingside f/g; queenside
  b/c/d — the **b-file empty but not necessarily unattacked**), and the king is
  **unattacked on origin, transit and destination** (kingside e/f/g; queenside e/d/c).
  The origin is a read-only `pos_is_in_check`; the transit (a one-square king step) and
  the destination (the actual castle) are probed on **one normalized scratch** (bounded
  stack-local) via the proven make/unmake — the caller's Position + history are untouched.
  The two non-castling APIs keep their contracts (they still exclude every castle); null
  moves and SEE remain later milestones.
- **Test** (`tests/movegen_castling_test.cpp`, new): the 19-fixture corpus (origin /
  transit / destination attacks, each obstructing path square, attacked-but-allowed
  b/rook squares, giving a check, a recorded EP) + rank/color mirrors; all 16 rights
  masks both sides + rights staying lost after king/rook return; all four castles' real
  make/unmake vs a cold rebuild (rights, EP, counters, key, exact restore);
  capacity/overflow (26/25/24 + canaries + non-castling propagation + empty spans);
  preservation/determinism; counter independence. The complete set is encoded exactly
  (the home-rook origins); the castle/EP/promotion flag + payload semantics are verified
  separately, so the castle encoding is never confused with a normal rook move. Wired
  into the Makefile (all three configs) + `make test`.
- **Docs:** PLAN cp3 marked done (cp4 = next); ARCHITECTURE movegen module updated to
  the three generators.

## Known discrepancies (surfaced, not hidden)
- The reference `blocked_b` corpus listed `a1b1`, which is illegal under its own FEN
  (`RN2K2R`: a knight occupies b1, blocking the a1 rook's rank ray) — a transcription
  artifact of the `blocked_c` pattern (whose b1 is empty). Independently verified, the
  correct set is 25 moves (no a1b1); the test carries the corrected set + an in-file note.
- The tree carries a **preserved, uncommitted** one-line correction to the cp2
  `quiet_block` fixture (`e1f1`→`e1f2`; a king move onto its own bishop was impossible).
  I did **not** fold that cp2-correlated edit into this cp3 commit (one commit per
  task), so a clean checkout of this commit retains the verbatim `e1f1` corpus and its
  one documented `quiet_block` discrepancy (the cp2 STATE.md surfaced it); the tree
  keeps the `e1f2` correction for the owner to commit or tell me to include.

## Latest checks (host: x86-64, 8 cores, 126 GiB, Debian, GCC 16.2.0-3)
- `make check-cpp20`: **green** (25 files, no non-C++20 constructs).
- Engine + tests build in **release / debug / sanitize** (exit 0 each; 0 warnings).
- `make test CONFIG=release`: all 17 suites pass **on the working tree** (which also
  carries the uncommitted `quiet_block` correction above).
- Sanitizer: with the documented `LD_PRELOAD=libasan.so` workaround all 17 binaries run
  fully with **0 ASan/UBSan** diagnostics. The *ordinary* `make test-sanitize` target is
  host-blocked (a pre-existing `/etc/ld.so.preload` → `libenable-3fg-drag.so` breaks the
  ASan loader order for every program) — environmental, unchanged by this checkpoint.
- **Clang not on PATH** → unrun; AArch64/Apple Silicon + RV64 remain unverified.

## Blockers
None for the code. The `blocked_b` corpus artifact and the ASan-launcher /
missing-Clang items are prompt-side / environmental, not code defects.

## Next (exactly one task)
**T005 cp4 — perft harness + differential (M1 exit).** `perft` **above** position+
movegen (the board core does not depend on movegen): ≤5 depths on startpos
(4,865,609) + ≥3 special-move FENs == trusted counts (fixture provenance recorded); a
differential legal-move check vs an independent trusted generator; make/unmake stays
exact under the generator.
