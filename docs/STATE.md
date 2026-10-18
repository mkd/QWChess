# QwenChess — current state

**Milestone:** `CPP20-3A` (done this commit) — bounded FEN + pseudo-legal move-buffer
interfaces, plus the CPP20-2B factual/build-policy corrections folded in.
(There is no T005/T009/T012/T013/T016 work in this repo; `PLAN.md` uses `T###` task
IDs and the `CPP20-*` migration checkpoints.)

**Last verified commit:** `d68b925` (CPP20-2B) is the parent; this commit is CPP20-3A.

## What this checkpoint did
- **New bounded `qwc` interfaces** (direct `<span>`/`<string_view>` includes; borrowed
  views, never stored): `fen_load(Position&, std::string_view, std::string_view* =
  nullptr)`, `fen_emit(const Position&, std::span<char>)`,
  `movegen_pseudo_noncastling(const Position&, std::span<Move>, std::size_t&)`. The
  existing pointer overloads remain as small **adapters** that validate their legacy
  null/signed cases and delegate to the same primary — there is exactly one parser,
  one emitter, one generator. The FEN parser is sized by the view (no `strlen`/assumed
  terminator; rejects empty + any NUL in the range); the move buffer checks capacity
  before every append.
- **Factual/build-policy corrections (CPP20-2B, folded in, no separate stop):** the
  2B handoff's misreported `MoveDelta` members / nonexistent types (`SearchState`/
  `SearchLimits`/`SearchResult`/`EpTarget`/`File`/`Rank`/`DIR`) / counter-constant
  types corrected to match the source; `AGENTS.md`'s stale "no `std::array`" rule
  fixed; `make check-c` reduced to a documented **legacy alias for `check-cpp20`**
  (the 2B token blacklist removed — it did not establish C++ correctness/portability);
  `make test-portable-bits` now builds **and** runs both `sq_bb_test` and
  `attacks_test`.
- **Focused regressions:** string_view/span FEN + movegen coverage (non-terminated
  valid input, a slice in surrounding junk, empty/default views, embedded + terminal
  NUL, truncation, trailing junk, preserved Position+history on failure, exact+1 /
  one-byte-short / empty emission, canaries + unchanged suffix, count reset, empty
  spans, diagnostics), the missing aggregate/extent `static_assert`s, and the
  two-suite portable-bits gate.

## Latest checks (this host: x86-64, 8 cores, 126 GiB, Debian GNU/Linux 13, GCC 16.1.0)
- `check-cpp20` + `check-c` (alias): **green** (23 files, no non-C++20 constructs).
- `make test CONFIG=release`: **193 PASS groups, 0 fail**, engine builds, **0
  warnings** under `-Wall -Wextra -Wshadow -Wconversion -Wwrite-strings`.
- `make test CONFIG=debug`: **193 PASS groups, 0 fail**.
- Sanitizer: the instrumented build compiles and all 15 suites **pass via the
  documented `LD_PRELOAD=libasan.so.8` + `detect_leaks=1` workaround** (0 ASan/UBSan/
  leak diagnostics). But the **ordinary `make test-sanitize` target fails on this
  host** (`/etc/ld.so.preload` → `libenable-3fg-drag.so`, a VM tool, breaks ASan's
  loader order for *every* program). Those are separate outcomes: workaround run
  passes, ordinary target does not.
- `make test-portable-bits`: **green** (sq_bb + attacks agree with the `<bit>` path).
- **Clang: not installed in this build environment** (no `clang`/`clang++`; no LLVM
  dirs) — all verification is under GCC. (The planning-phase check had reported clang
  21.1.0 in a different context; it is absent here, so no Clang build was run.)

## Blockers
- None for CPP20-3A. The two host-level items above (ASan loader preload; missing
  Clang) are environmental, not code defects, and are reported separately.

## Next (exactly one task)
`CPP20-3B`: the clock contract (steady_clock epoch is not guaranteed nonnegative),
coherent human-facing output cleanup, `std::numeric_limits` where appropriate (no
reflexive `INT64_MAX` replacement), the remaining per-module documentation
(ARCHITECTURE.md policy), and validating the Clang build **where it is installed**
(AArch64/Apple Silicon + RV64 reported separately). Then `CPP20-4` (final audit +
build profiles + same-ISA comparison + C++26 readiness), then T005 cp2.
