# QwenChess — current state

**Milestone:** `T005 cp4` (done this commit) — the **M1 exit**: the perft harness +
the independent move-generation differential. M1 (board core) is complete; **M2 is next**.

**Last verified commit:** `ad19da0` (legal-movegen test fix) is the parent; this commit
is T005 cp4.

## What this checkpoint did
- **Perft harness** (`src/diagnostics/perft.{hpp,cpp}`, a `qwcperft` tool,
  `tests/perft_test.cpp`): `perft` counts pseudo-legal leaves **above** position+movegen
  (the board core stays movegen-independent). `make test-perft`: startpos ≤5 depths
  (4,865,609 at d5) + 3 special-move FENs == trusted counts (provenance recorded in the
  test); bounded perft stays in the ordinary suite.
- **Independent movegen differential** (`tools/movegen_diff.py` +
  `tools/position_probe.cpp`, test-only, pinned `python-chess`): the reference generates
  encoded moves + expected legal sets/FENs; the C++ probe links QWChess and reports its
  legal sets + FENs; the driver compares **1672 positions / 1376 make-unmake
  transitions** (startpos + 4 special roots + targeted castles / EP / promos /
  capture-promos / mate / stalemate / rights). `make test-differential`.
- **Build fix:** the sanitize config now links `-static-libasan`, so `make test-sanitize`
  runs on this host. The setuid `/etc/ld.so.preload` hook (`libenable-3fg-drag.so`) broke
  the *dynamic* ASan loader order; static ASan sidesteps it (the cp3 session had only a
  manual `LD_PRELOAD` workaround). ASan/UBSan-clean across the suite + the differential.

## Latest checks (x86-64, 8 cores, 126 GiB, Debian, GCC 16.2)
- `make check-cpp20`: green (29 files, no non-C++20 constructs).
- `make test` (release/debug/sanitize): all suites pass, exit 0, 0 ASan/UBSan.
- `make test-perft` (release/debug/sanitize): full-depth gate passes.
- `make test-differential` (release/debug/sanitize): 1672 positions / 1376 transitions
  -> PASS in each.
- Clang / AArch64 / Apple Silicon / RV64 remain unverified (not on this host).

## Blockers
None for the code. Non-x86 + Clang hosts remain unrun (prompt-side / environmental).

## Next (exactly one task)
**M2 / T006 — labelled temp Material eval + responsive UCI** (search plumbing only, never
a NNUE fallback): a command-reader thread independent of the single search worker + atomic
cancellation; UCI init / options / position / go(depth,movetime,infinite) / stop / quit /
newgame; serialized stdout; correct mate / stalemate / 50-move handling.
