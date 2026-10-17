# QwenChess Plan

Milestone checklist with task IDs. Detailed steps only for near-term work; the
rest is an ordered backlog with objective/dependencies/exit-gate. Work is serial
(one reviewable change per task, usually one module + a few files + focused tests).

Milestones: **M0** scaffold · **M1** board correctness · **M2** minimal UCI+search ·
**M3** exact scalar NNUE · **M4** incremental NNUE · **M5** measured strength ·
**M6** SIMD/platform · **M7** SMP/advanced. Plus the cross-cutting **C++20
Modernization Workstream** (CPP20-1…7; see that section) — a language refactor, not a
functionality milestone.

> **Language note:** M0–M7 below were authored in **C17** and their checkpoint
> records quote C17 files/flags historically. The shipped codebase is now **C++20**
> (CPP20-1); read those C17 references as history, not the current language.

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
  - **Still open in M1:** **T005 (movegen + perft)** is the last M1 task — T003 and
    all of T004 (checkpoints 1–5: ordinary, EP, castling, promotion, null + history)
    are done and verify explicit make/unmake fixtures; seeded legal sequences land
    in T005 once movegen exists (which is when perft can run).

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
  - **Status:** **all five checkpoints done.** cp1 (ordinary moves) committed
    (`00a579c`); cp2 (en-passant capture), cp3 (orthodox castling) and cp4
    (promotion/underpromotion) implemented in `e71c4b7`, their three test suites
    committed by the cp4 reconcile; **cp5 (the null move + repetition history)**
    landed by this task: `pos_make_move` / `pos_unmake_move` + `StateInfo`/
    `MoveDelta` cover quiet / ordinary capture / pawn single+double push / ordinary
    pawn capture / EP capture (3-edit delta) / orthodox castling (4-edit delta) /
    promotion + underpromotion (2-edit delta), and a distinct `pos_make_null_move`
    (zero edits; flip side; clear EP; preserve both counters; exact key) plus the
    caller-owned, `MAX_PLY`-uncapped `StateInfo.prev` history chain and the
    `pos_repetition_count` production query (a null node counts 0 and is a walk
    boundary). T004 is complete. See STATE.md.
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

Checkpoints (each a separate compilable, testable, committable unit):
- **cp1 — non-castling pseudo-legal generation. DONE (this commit).** A frozen
  `movegen_pseudo_noncastling` API in a new `src/movegen/` module (depends on
  core+position only). Emits every non-castling pseudo-legal candidate — geometric
  moves/captures, all four promotions + capture-promotions, and structural en
  passant (recorded target + enemy pawn + empty landing + adjacent captor; no king
  safety) — via the existing bitboards/attack tables, no heap, no Position copy,
  no make/unmake, no rebuild. Tests: 14 independent candidate sets + rank/reflected
  mirrors, exact startpos (20), deliberate king-safety boundary cases, blocked-first
  pawn step, capacity/overflow + canaries, determinism + live history, counter
  independence, and per-candidate make/unmake round-trips. **cp1 is pseudo-legal,
  not legal** (kings may move to attacked squares; a pinned captor's EP stands).
- **cp2 — own-king safety (legal gen).** (next M1 task) For each pseudo-legal
  candidate, save the **original mover's color**, apply the move (`pos_make_move`) and
  unmake it, testing that mover's king against **all enemy geometric attacks on the
  resulting board — including attacks from pinned enemy pieces**; keep the candidate iff
  the mover's king is unattacked. **Retain all king-safe moves** (quiet blocks,
  sacrifices, EP/promotion evasions). A **pure correctness filter** — no SEE/material
  pruning in legal generation (SEE *ordering* is a later M5 strength feature). The
  deliberately **unsafe** `mg.boundary` pseudo-candidates (a king stepping to an
  attacked square) must be **absent** from legal output. Per-candidate make/unmake is
  permitted (the blanket ban is removed).
- **cp3 — castling.** internal encoding is **king-origin → rook-origin** (e1h1/e1a1/
  e8h8/e8a8); the move is applied as king→g/c and rook→f/d. Legal checks: the right
  exists; the squares between king and rook are empty (kingside f/g; queenside b/c/d,
  the **b-file empty but not necessarily unattacked**); and the **king is unattacked on
  origin, transit and destination** (kingside e/f/g; queenside e/d/c). Per-candidate
  make/unmake is permitted (the blanket ban is removed).
- **cp4 — perft harness + differential (M1 exit).** `perft` **above** position+
  movegen (the board core does not depend on movegen); ≤5 on startpos
  (4,865,609) + ≥3 special-move FENs == trusted counts (fixture provenance
  recorded); a differential legal-move check vs an independent trusted generator;
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

## C++20 Modernization Workstream (cross-cutting refactor of the existing code)

A language transition of the **shipped** engine from C17 to C++20 (the former deferred
roadmap item "a", now active). One reviewable change per task; behavior, the full test
suite and the C17 reference (`93d3c77`) are preserved at every step. It runs as a
**parallel track** to M0–M7 (which add functionality); the M1 board work resumes at
T005 cp2 after the CPP20 phases. **Four phases with bounded sub-checkpoints** (C++20 is
the current build baseline; C++26 is the intended direction to *evaluate* —
REQUIREMENTS.md BUILD-1). Approved product requirements live in `docs/REQUIREMENTS.md`.

### CPP20-1 — language/build foundation  ✅ DONE
Renames (`.c`→`.cpp`, `.h`→`.hpp`); Makefile (`CXX`/`CXXFLAGS`, `-std=c++20`,
`CXX=g++`, drop C-only warning flags, `build/cpp20/`, `check-c17`→`check-cpp20`);
`static_assert`/`nullptr`/C++ standard headers/`std::` qualification; `using` aliases
+ plain `struct` aggregates (no constructors); bitboard → `<bit>` (portable fallback
retained); clock → `std::chrono::steady_clock`; `POS_HM_MAX`/`FEN_BUF_SIZE`/`BB_FULL`→
`constexpr`; five enum/non-enum ternary warnings fixed at the boundary.
**Exit gate (met):** `check-cpp20` over 23 files; release + debug builds 0 warnings;
15/15 release + 15/15 debug; portable bit path green; struct size/alignment unchanged;
docs flipped off C17. (Sanitizer normal target is host-runner-limited — see STATE.)

### CPP20-2A — scoped enums + closeout  ✅ DONE (this commit)
Convert `Color`, `PieceType`, `Piece` to `enum class X : u8` and `MoveFlag` to
`enum class : u32`, preserving every numeric value, `ALL_PIECES`/`NO_*` sentinels and
all static_asserts; the `*_NB` counts move to `constexpr int` (a **design choice** — a
count is not a domain value we keep in the enum; not a language restriction). Every
value/byte/table-index/raw-bit boundary is re-expressed with `to_under`/`static_cast`;
domain values stay typed. `Square` keeps the one-byte `u8` alias.
**Closeout (this commit):** the Makefile `test` target honors `CONFIG`
(release/debug/sanitize; an unknown value is a hard error) and `test-run`/`all-config`
are phony; `scaffold_test` gains the scoped-enum (scoped + no-implicit-conversion + exact
underlying type), frozen-value and aggregate trivially-copyable/standard-layout + x86-64
size/alignment assertions; `size_t`→`std::size_t` + direct `<cstddef>`; comment
corrections (`<bit>` zero returns the bit width, not UB; "null move"; `*_NB` is a design
choice; `std::numeric_limits` is a facility; the steady_clock epoch is not guaranteed
nonnegative); the engineering policy (ARCHITECTURE.md) and requirements register
(REQUIREMENTS.md) are recorded.
**Exit gate (met this commit):** `check-cpp20` 23 files; release + debug + sanitize
builds 0 warnings; 15/15 tests, 184 PASS, 0 fail in each config; `test CONFIG=debug`
runs the debug binaries and `test CONFIG=sanitize` the instrumented ones (verified on
the compile **and** link commands); size/alignment contract unchanged (Position 248/8,
StateInfo 48/8, MoveDelta 20/4, SquareEdit 3/1). (The sanitizer *run* on this host is
constrained by libasan loader ordering — see STATE; the instrumented binaries build and
run cleanly via a documented preload.)

### CPP20-2B — project namespace, naming, typed constants, fixed storage  ✅ DONE
Introduce the project namespace; apply the frozen naming conventions (PascalCase domain
types, snake_case functions/variables, UPPER_SNAKE constants/enumerators —
ARCHITECTURE.md Engineering policy); convert the remaining typed constants and fixed-size
storage to `std::array` (e.g. `Square mailbox[64]`); preserve behavior and document the
migrated surface. **No language-standard increase, no new CPU flags, no ISA.** The M1
board work resumes at T005 cp2 after 2B.

Landed as five deliberately-ordered, independently-verifying batches (each compiled +
tested green before the next):
- **(1) names + namespaces:** domain types into `namespace qwc` (`Square`/`Piece`/`Color`/
  `PieceType`/`MoveFlag`/`Move`/`Value`/`File`/`Rank`/`DIR`/`EpTarget` + `Position`/
  `StateInfo`/`MoveDelta`/`SearchState`/`SearchLimits`/`SearchResult`); PascalCase types,
  snake_case functions/variables, UPPER_SNAKE constants/enumerators; the `enum`→
  `enum class` byte/value boundary re-expressed with `to_under`/`static_cast`; tests use
  `using namespace qwc`.
- **(2) constants:** the numeric constants frozen as typed `constexpr` (`POS_HM_MAX`
  u16, `POS_FM_MAX` u32, `FEN_BUF_SIZE` std::size_t, `DIR_NB` int, `MAX_PLY` int,
  `MAX_PLY_STACK` size_t) — the raw `0xFFFF`/`0xFFFFFFFF`/`1<<30` spellings removed.
- **(3) storage:** every fixed-size *data* array → `std::array` (Position `mailbox`/
  `by_piece`/`by_color`, `MoveDelta.squares`/`pieces`/`occ_delta`/`king_sq_delta`,
  attack/zobrist/fen tables+scratch, the TState test struct); the C-string/move-buffer
  boundary takes `.data()`/`.size()`; byte-ops become typed (`fill`, `==`, value-init,
  value-copy) with no `sizeof T*sizeof T` or `memcmp`-for-equality; the **retained**
  `char out[3]`/`char dst[512]`/`const char**` **function parameters** stay pointers.
- **(4) bitboards:** `bitboard.hpp` documented as endianness-agnostic (bit *index*
  convention, not byte order) + a 64-bit-width `static_assert` trap; the `QWC_FORCE_
  PORTABLE_BITS` fallback gains a reproducible `make test-portable-bits` target.
- **(5) build matrix + verification:** `make clean`/`-j`/`debug`/`release`/`sanitize`/
  `check-c`/`check-cpp20`/`test`/`test-sanitize` all work; a `check-c` C-residue gate
  (no C-only constructs / C-compiler extensions) complements `check-cpp20`.

**Exit gate (met this commit):** `check-c` (no C residue) + `check-cpp20` (23 files,
no non-C++20 constructs) green; release build 0 warnings under `-Wall -Wextra -Wshadow
-Wconversion -Wwrite-strings`; **186 PASS / 0 fail in all three configs** (release,
debug, and sanitize — the sanitize *run* via the documented `LD_PRELOAD=libasan.so.8`
workaround for this host's `/etc/ld.so.preload`, 0 ASan/UBSan issues); the `static_assert`
layout contract unchanged (Position 248/8, StateInfo 48/8, MoveDelta 20/4, SquareEdit
3/1); **0 raw C array declarations in `src/`** (all data arrays are `std::array`); **0 new
`<cstdint>` includes**. The `QWC_FORCE_PORTABLE_BITS` portable-bits path is tested.

### CPP20-3 — interfaces/ownership + platform services + output + documentation
Interface/ownership cleanup: references/pointers for output parameters; `std::span`/
`std::string_view`/`std::string` and constructors/`operator==`/`constexpr` only where
they genuinely simplify an interface. The clock **contract design review** (recorded in
2A; steady_clock epoch is not guaranteed nonnegative). `std::numeric_limits` is a proper
C++ facility (keep where appropriate); replace `INT64_MAX`-style `<climits>` macros only
when a clearer typed expression helps. Coherent human-facing output cleanup + the
remaining per-module documentation (ARCHITECTURE.md policy). **Validate** the GCC and
Clang builds on x86-64 (both green incl. `check-cpp20`); report AArch64 (incl. Apple
Silicon) and RV64 support **separately** — do not claim untested architecture support.

### CPP20-4 — final audit + build profiles + same-ISA comparison + C++26 readiness
Final migration/documentation audit (no C-only constructs, no implicit narrowing, a
defined memory model, portability limits documented). **Build-profile foundations**
(REQUIREMENTS.md BUILD-1): explicit compiler / C++ standard / target arch-OS-ABI / CPU /
toolchain selection with native, portable-distribution and cross modes + a build-info
report; the **C++26 readiness review** (compiler AND standard-library feature checks,
Apple Clang vs upstream — a flag being accepted is *not* proof the feature works). A
**same-ISA** performance comparison of the identical C baseline `93d3c77` against the
C++20 code (same `-march`/optimization, no new ISA) on available hardware, reporting
generated code, timings, memory, NPS and instruction mix — **actual ISA kernels/tuning
are M6**, so no ISA change here. The **cross-build matrix** (REQUIREMENTS.md BUILD-2)
validates declared host/target cells in stages; absent hardware/toolchains are
**reported, not invented.** Do not alter the determinism/ownership/score/synchronization
contracts merely because C++ permits it. (The old CPP20-5 ISA experiment is absorbed
into M6; old CPP20-6 platform work splits across CPP20-3/4.)

---

## Deliberately deferred (roadmap — scheduled only on later, measured need)
- **C++20 migration (a).** Now the active **C++20 Modernization Workstream** above
  (CPP20-1 + CPP20-2A + CPP20-2B done, CPP20-3 next) — no longer deferred. Its continuing rules:
  gate on C++20 + the platform standard library, keep the full correctness/NNUE/search
  regression suite green, record the new build + sanitizer coverage, and keep UCI,
  determinism and the single-worker mode identical.
- **Targeted platform/performance tuning (b).** Only after a measured bottleneck on
  the actual execution CPU: hot-path cache alignment/branch layout, vector width and
  instruction sets, LTO/PGO, and a benchmark-driven choice between a standard C
  library and a purpose-built allocator. No speculative optimization and no
  unmeasured micro-tuning. (NNUE SIMD kernels are the M6/T-scope items; this covers
  the general platform layer.)

---

## Ordered backlog (IDs reserved)
C++20 track: `CPP20-1 ✅ → CPP20-2A ✅ → CPP20-2B ✅ → CPP20-3 ← next → CPP20-4`
(2A = scoped enums + closeout; 2B = namespace + naming + typed constants + `std::array`
storage + bitboards + build matrix; the old CPP20-5 ISA experiment is absorbed into M6).
Milestones: `T005 cp2` (legal movegen) → `cp3` (castling) → `cp4` (perft = M1 exit)
→ `T006..T007` (M2) → `T008..T010` (M3) → `T011` (M4) → `T012..` (M5 strength)
→ `T…` (M6) → `T…` (M7).
Reordering notes: M2 search runs on the **labelled temp eval**, so search and NNUE
progress independently and merge at M5. The C++20 track is a cross-cutting refactor that
may interleave with the M-track; CPP20-4 (build profiles + same-ISA comparison) must
land before M6 (SIMD) so the performance comparison is against a settled C++ baseline.
