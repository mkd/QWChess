# QwenChess — current state

**Milestone:** M2 — the **synchronous search** (`do_search`) is done (this commit).
M1 (board core) was complete at the last checkpoint; the responsive UCI is next.

**Last verified commit:** the T005 cp4 (M1 exit, perft + differential) is the parent;
this commit is the search.

## What this checkpoint did
- **`src/search/search.{hpp,cpp}`** — `do_search` (the synchronous engine search, above
  position+movegen, no I/O): a per-search `SearchWork` arena (state stack + the
  `MAX_PLY` move buffers + the per-ply `SearchData`, all preallocated once; no per-node
  heap) + an iterative-deepening **fail-soft negamax** + a **quiescence** (in check: every
  evasion; otherwise the captures with the static score as the stand-pat, `MAX_PLY`-bounded).
  Legal moves only (`movegen_legal`); mates (`mated_in(ply)`, root-relative), stalemate (0),
  the **draw policy** (3-fold / halfmove≥100 → 0, checked after mate/stalemate), a node
  counter, and a `std::atomic<bool>` stop. Interrupted searches keep the last fully
  completed iteration (move/score/PV) + the pre-selected legal fallback; a root with no
  legal move returns `0000`. Limits: depth / node budget / movetime-deadline / infinite.
- **`src/search/material.{hpp,cpp}`** — the **labelled temporary** Material eval (piece
  values + a bounded mobility term; read-only, allocation-free, never a NNUE fallback; its
  range is `static_assert`-ed inside the reserved material interval). See T006.
- **`tests/search_test.cpp`** — the gate: mate-in-1 / checkmate-at-root / stalemate /
  3-fold / 50-move / immediate-stop / node-budget / quiet-startpos / quiescence(hanging
  queen), each with legality + sign + determinism + the last-completed-iteration contract.

## Latest checks (x86-64, 8 cores, 126 GiB, Debian, GCC 16.2)
- `make check-cpp20`: green (35 files, no non-C++20 constructs).
- `make test` (release): all suites pass, exit 0, 377 checks.
- `make test-perft` (release): full-depth gate passes (the movegen is untouched).
- `make CONFIG=sanitize` + `LD_PRELOAD=libasan`: search / movegen-legal / make-unmake all
  run clean (no ASan/UBSan diagnostics). Clang / AArch64 remain unverified (not on host).

## Next (exactly one task)
**M2 / T006 — responsive UCI protocol**: a command-reader thread independent of the single
search worker + atomic cancellation; `uci`/`option`s/`position [fen|startpos moves…]`/
`go depth|movetime|infinite`/`stop`/`quit`/`new`; `info` + `bestmove`; serialized stdout;
advertising only implemented options; mate/stalemate/`0000` output. `do_search` is ready
to call. (TT + time-management land as the next search-isolated step, T007.)
