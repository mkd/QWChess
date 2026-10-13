# QwenChess

A standalone, high-performance **UCI chess engine in C++20**, built around
Stockfish 19's NNUE architecture and compatible with its original `.nnue` files.

The long-term goal is research toward the strongest C++ chess engine; it is a
measured objective, not a promise. Correctness and reproducibility are the
prerequisites; measured playing strength per unit of thinking time is the target.

## Constraints (invariant)

- All shipped engine logic, including NNUE inference, compiles as **C++20**.
  The C++ standard library is permitted at the implementation level; no external
  evaluator process, no LLM/remote calls at play time, no exceptions, no RTTI.
- The upstream Stockfish 19 **C++** executable may serve as a *test oracle* and
  opponent, never as QwenChess's runtime backend.
- Standard chess first, deterministic single search worker. A practical path to
  Chess960, multiple workers, tablebases and stronger search is preserved without
  implementing them prematurely.
- GPL-3.0-or-later; upstream provenance is recorded in `PROVENANCE.md`.

## Layout

```
src/       engine sources (C++20) core / position / movegen / nnue / search / engine / platform
tests/     executable tests (C++)
tools/     net fetch/verify, match + perft helpers (shell/python, not shipped)
docs/      ARCHITECTURE.md  NNUE_COMPATIBILITY.md  PLAN.md  STATE.md
AGENTS.md  agent rules for AI-assisted sessions (read this first)
```

The `reference/` (upstream Stockfish checkout) and `networks/` (fetched net)
directories are gitignored and isolated from the normal source tree.

## Build & test (Debian, g++ by default; `CXX=clang++` optional; no CMake)

```sh
make                 # release engine        -> build/cpp20/release/bin/qwenchess
make debug           # debug engine          -> build/cpp20/debug/bin/qwenchess
make sanitize        # ASan+UBSan engine     -> build/cpp20/sanitize/bin/qwenchess
make test            # build + run all tests (release flags)
make test-sanitize   # build + run all tests under ASan/UBSan
make check-cpp20     # strict -std=c++20 -pedantic-errors gate over all sources
make net             # fetch + verify the NNUE network into networks/
make test-net        # net-checker self-test (rejects bad, accepts good)
make clean           # remove all build output
```

The normal build works fully offline and never requires the network download.

## Status

**C++20-1 (this checkpoint):** the whole shipped codebase has been migrated from
C17 to C++20 (`.cpp`/`.hpp`, `g++`, `build/cpp20/`, `make check-cpp20`) with the
compact representation, behavior and the full test suite preserved.

M1 (board correctness) is the functional track and remains current work — the board
core is complete and tested: fixed-width types, bitboards, move encode/decode,
portable attacks, Zobrist hashing, the authoritative `Position` (with
representation-boundary validation), and strict FEN load/emit (en-passant
record/normalization, draw counters bounded by storage). This is a board/foundation
layer: the engine binary is a scaffold and is **not** a playable UCI engine. No
search, UCI protocol, NNUE evaluation, or playing strength is implemented yet. See
`docs/STATE.md` for the current milestone and `docs/PLAN.md` for the roadmap.
