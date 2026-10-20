# QwenChess — current state

**Milestone:** `T005 cp2` (done this commit) — non-castling **legal** move generation
(`movegen_legal_noncastling`): the cp1 pseudo-legal candidates filtered for own-king
safety via a scratch probe. M1 continues with **cp3 (castling)** next.

**Last verified commit:** `bf3840e` (CPP20-3B) is the parent; this commit is T005 cp2.

## What this checkpoint did
- **Legal generator** (`src/movegen/movegen.{hpp,cpp}`): `movegen_legal_noncastling(const
  Position&, std::span<Move>, std::size_t&)`. Saves the **original mover's** color,
  applies each pseudo-legal candidate to **one scratch copy** of the Position per call
  (bounded stack-local; the caller's Position + history are untouched), tests that
  mover's king with the **geometric** `pos_is_in_check` (pinned enemy pieces included),
  reverse-by-`pos_unmake_move`, keeps the candidate iff the mover's king is unattacked.
  **Retains all king-safe moves** (blocks, sacrifices, EP/promotion evasions, all four
  promotions); excludes castling + null. Counters are normalized on the scratch (a valid
  recorded EP already requires halfmove 0), so the *game* overflow guard is intact.
- **Test** (`tests/movegen_legal_test.cpp`): the 22-position prompt-supplied legal
  corpus + rank/color mirrors (complete encoded Move sets + EP/promo/no-castling flag
  semantics), capacity/overflow + canaries (mate/stalemate succeed on an empty span),
  preservation/determinism (Position incl. history + live records untouched), counter
  independence (u16 boundaries, both colors, EP retains halfmove 0), and a per-move
  actual make / child-consistency / cold-rebuild / unmake round-trip.
- **Docs:** AGENTS/REQUIREMENTS/PLAN/ARCHITECTURE record that **C++26 is deferred**
  pending an explicit request (C++20 is the baseline; the broad build-profile work
  still follows M2). Movegen header contract corrected (legal = scratch probe; castling
  is cp3).

## Known discrepancy (surfaced, not hidden)
The prompt-supplied `quiet_block` fixture (`k3r3/8/8/8/8/8/8/4KB2 w - - 0 1`) lists
`e1f1` — a king move onto its **own** bishop on f1, which is impossible. The engine
correctly produces `e1f2`; its mirror shows `e8f7` vs the corpus `e8f8`. This is a
one-character typo in the prompt-supplied data, **not** an engine defect. Per the task
rule ("never replace a fixture to make the test pass") the corpus is kept verbatim and
this one assertion is the sole test failure.

## Latest checks (host: x86-64, 8 cores, 126 GiB, Debian, GCC 16.2.0-3)
- `make check-cpp20`: **green** (24 files, no non-C++20 constructs).
- Engine builds in **release / debug / sanitize** (exit 0 each); UCI scaffold runs.
- `make test CONFIG=release` and `CONFIG=debug`: all 14 pre-existing suites pass; the
  new legal suite passes **except** the `quiet_block` corpus discrepancy above.
- Sanitizer: with the documented `LD_PRELOAD=libasan.so.8 detect_leaks=1` workaround all
  16 binaries run **fully** with **0 ASan/UBSan/leak** diagnostics. The *ordinary*
  `make test-sanitize` target is host-blocked (pre-existing `/etc/ld.so.preload` →
  `libenable-3fg-drag.so` breaks ASan loader order for *every* program, incl. a trivial
  fresh one) — environmental, reported separately, unchanged by this checkpoint.
- **Clang not on PATH** → unrun; AArch64/Apple Silicon + RV64 unverified.

## Blockers
None for the code. The `quiet_block` corpus typo is a documented prompt-side data
error (surfaced per the task); the ASan-launcher + missing-Clang items are
environmental, not code defects.

## Next (exactly one task)
**T005 cp3 — orthodox castling generation**: internal encoding **king-origin → rook-
origin** (e1h1/e1a1/e8h8/e8a8), applied as king→g/c + rook→f/d. Legal: the right
exists; the between-squares are empty (kingside f/g; queenside b/c/d — the **b-file
empty but not necessarily unattacked**); the **king is unattacked on origin, transit and
destination** (kingside e/f/g; queenside e/d/c). Per-candidate make/unmake is permitted.
