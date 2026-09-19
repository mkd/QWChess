# QwenChess

A standalone, high-performance **UCI chess engine in C17**, built around
Stockfish 19's NNUE architecture and compatible with its original `.nnue` files.

The long-term goal is research toward the strongest C chess engine; it is a
measured objective, not a promise. Correctness and reproducibility are the
prerequisites; measured playing strength per unit of thinking time is the target.

## Constraints (invariant)

- All shipped engine logic, including NNUE inference, compiles as **C17**.
  No C++ runtime, no external evaluator process, no LLM/remote calls at play time.
- The upstream Stockfish 19 **C++** executable may serve as a *test oracle* and
  opponent, never as QwenChess's runtime backend.
- Standard chess first, deterministic single search worker. A practical path to
  Chess960, multiple workers, tablebases and stronger search is preserved without
  implementing them prematurely.
- GPL-3.0-or-later; upstream provenance is recorded in `PROVENANCE.md`.

## Layout

```
src/       engine sources (C17)   core / position / movegen / nnue / search / engine / platform
tests/     executable tests (C)
tools/     net fetch/verify, match + perft helpers (shell/python, not shipped)
docs/      ARCHITECTURE.md  NNUE_COMPATIBILITY.md  PLAN.md  STATE.md
AGENTS.md  agent rules for AI-assisted sessions (read this first)
```

The `reference/` (upstream Stockfish checkout) and `networks/` (fetched net)
directories are gitignored and isolated from the normal source tree.

## Build & test (Debian, GCC by default; `CC=clang` optional; no CMake)

```sh
make                 # release engine        -> build/release/bin/qwenchess
make debug           # debug engine          -> build/debug/bin/qwenchess
make sanitize        # ASan+UBSan engine     -> build/sanitize/bin/qwenchess
make test            # build + run scaffold test (release flags)
make test-sanitize   # build + run the test under ASan/UBSan
make net             # fetch + verify the NNUE network into networks/
make test-net        # net-checker self-test (rejects bad, accepts good)
make clean           # remove all build output
```

The normal build works fully offline and never requires the network download.

## Status

Early scaffold (task T001): foundational types, a monotonic clock, a build
system, and a passing scaffold test. The engine binary identifies itself as a
scaffold and is **not** a playable UCI engine yet. See `docs/STATE.md` for the
current milestone and `docs/PLAN.md` for the roadmap.
