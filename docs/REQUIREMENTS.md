# QwenChess — Requirements Register

Durable register of approved product requirements, recorded as **planned** (not
implemented). Each item has a stable ID, the milestone that delivers it, the
acceptance evidence required, and its current status. This is a *requirements*
document: it fixes *what* must be true and *how it will be evidenced*, not *how*
the code is written (that lives in `ARCHITECTURE.md`). Refine wording freely;
preserve the ID/milestone/acceptance/status distinctions. Linked from `AGENTS.md`
and `PLAN.md`.

Statuses: **planned** (not started) · **in progress** · **done** (with evidence).
Nothing below is implemented yet except where a milestone has shipped it.

---

## BUILD-1 — language and platform capability  *(planned)*

**Milestone:** CPP20-4 (build-profile foundations); kernels/tuning are M6.
**Requirement:**
- A Makefile workflow with explicit **compiler, C++ standard, target arch/OS/ABI,
  CPU and toolchain** selection. Plan **C++26** with **compiler AND standard-library
  feature checks** (Apple Clang vs upstream Clang; gcc/libstdc++ vs clang/libc++).
  Keep `-std=c++20` in the current checkpoint. **A flag being accepted is not proof
  that all C++26 features work.**
- Three distinct modes:
  1. **Native** — discover supported local CPU + OS capabilities, select an
     implemented backend and suitable compiler flags, and report the choice.
  2. **Portable distribution** — build for a documented minimum target ISA, or use
     correctly isolated runtime-dispatched kernels with a safe baseline path.
  3. **Cross** — explicit target/toolchain/sysroot + user-selected target
     capabilities; never infer the destination CPU from the build host, and never
     execute target probes without an explicit runner.
- Target roadmap: x86-64 Intel/AMD; AArch64 (incl. Apple Silicon and other
  ARMv8/ARMv9); RV64; and a **separate** ARMv7/AArch32 feasibility + implementation
  task. **macOS is not synonymous with ARM.** The current `sizeof(uintptr_t)==8`
  assertion explicitly excludes 32-bit ARM: do not claim ARMv7 support or remove the
  assertion alone as a "port." Audit pointer size, ABI, atomics, library support and
  memory limits first.
- Feature selection must distinguish **compiler availability / CPU capability / OS
  support / an actually-implemented kernel**. Candidates: x86 POPCNT/SSE/AVX2/VNNI/
  AVX-512/BMI2; ARM NEON/dot-product and SVE **only where available** (ARMv7 NEON is
  conditional; Apple chips must not be assumed to provide SVE); RISC-V bit-manip /
  vector extensions. **SSE is an instruction family; SEE is a chess search
  technique.** Do not infer every extension from an architecture name, and do not
  assume the widest vector path is fastest.
- For NNUE kernels, retain a scalar reference + exact integer-pipeline parity before
  benchmarking. Build flags alone do not create a correct optimized backend.

**Acceptance evidence:** a build-information report (effective flags, toolchain,
target, enabled backend); the native/portable/cross mode selection is observable and
correct per host; C++26 readiness is established by feature *checks* (not by a flag
being accepted); per-target correctness + native performance are recorded separately.
**Status: planned.**

## BUILD-2 — cross-compilation and reproducibility  *(planned)*

**Milestone:** CPP20-4 (cross-build matrix); exercised further in M6/M7.
**Requirement:**
- The build design allows supported build-host/target combinations when the required
  **compiler, linker, target C++ library, sysroot/SDK and dependencies** are supplied.
  Expose these inputs cleanly and fail with actionable missing-prerequisite messages.
  "Any host to any target" is a **design goal**, not a claim that Make supplies
  unavailable SDKs or compiler backends.
- Keep artifacts separate across **target, ABI, compiler, language standard,
  configuration and optimization profile** — or use a reliable configuration
  fingerprint that forces a rebuild. Include a build-information command/report
  (effective flags, toolchain, target, enabled backend).
- Validate a declared CI matrix **in stages**: compile/link; execution under an
  explicitly named emulator where useful; native correctness; native performance.
  **Emulated success is not native performance evidence.** Do not silently substitute
  host libraries or host flags.

**Acceptance evidence:** each matrix cell records its inputs and stage result; the
build-info report is reproducible; no silent host-flag/library substitution.
**Status: planned.**

## PERF-1 — fastest correct builds  *(planned)*

**Milestone:** M6 (measured target-specific optimization); foundations in CPP20-4.
**Requirement:**
- Benchmark appropriate candidates — **`-O2` vs `-O3`**, target tuning, supported
  LTO, representative PGO. Keep **portable and CPU-specific binaries distinct.** Do
  not stack all flags blindly; do not assume `-O3`/AVX-512/PEXT always wins. **No
  default `-Ofast`/`-ffast-math`** or relaxed-race assumptions that undermine
  correctness.
- An explicit **tuning/benchmark target** with cached results keyed to
  hardware/toolchain/build/benchmark identity; normal builds must not rerun an
  exhaustive tuning pass. Train PGO on representative engine workloads on a suitable
  target and verify the final optimized binary.
- Compare **correctness, throughput, memory and playing strength separately.**
  Search changes need controlled matches against pinned opponents on comparable
  hardware. Native builds may sacrifice binary compatibility with older CPUs while
  retaining portable source. **Do not invent NPS** for a scaffold.

**Acceptance evidence:** controlled before/after measurements reporting variation,
hardware, compiler, flags, memory; correctness preserved; a strength claim is backed
by a match (M5), not by NPS alone.
**Status: planned.**

## UCI-1 — complete protocol coverage  *(planned)*

**Milestone:** M2 (core UCI); extended in M7.
**Contract source:** the published UCI specification.
**Requirement:**
- A **coverage matrix** for commands, parameters, responses and options: mandatory
  behavior, optional capabilities, supported extensions, and genuinely inapplicable
  historical mechanisms.
- Account for `uci`, `debug`, `isready`, `setoption`, `register`, `ucinewgame`,
  `position`, `go`, `stop`, `ponderhit`, `quit`; all specified `go` limits,
  clocks/increments, `searchmoves`, and pondering semantics; handshake, readiness,
  `info` and `bestmove` responses. **Optional registration/copy-protection must not
  cause this open-source engine to invent registration requirements.**
- A **responsive reader independent of search.** Specify state transitions,
  EOF/`quit` cleanup, immediate `isready` during search, pending initialization
  barriers, **exactly one** appropriate `bestmove` per completed/stopped search,
  robust input parsing, and serialized complete output lines. Preserve option
  names/values containing spaces and paths. Handle unknown input as specified, with
  no unsolicited non-protocol stdout.
- An **option registry** advertising only working features, with truthful defaults,
  types and bounds. Implement the supported option types; document optional standard
  capabilities and common extensions. Plan Ponder, MultiPV, `UCI_Chess960`,
  `UCI_AnalyseMode`, Move Overhead, EvalFile, and strength/WDL **where their
  semantics are actually implemented** and, when needed, calibrated. **Do not fabricate
  Elo/WDL behavior** or promise every proprietary engine option. Legacy tablebase/book
  options are conditional on their actual backends.

**Acceptance evidence:** automated protocol transcripts; malformed-input cases;
stop/readiness responsiveness; search-lifecycle tests; and integration with a real
GUI/match runner. **"The binary prints its name" is not UCI validation.**
**Status: planned.**

## CONSOLE-1 — commands in the same input loop  *(planned)*

**Milestone:** M2 (with responsive UCI).
**Requirement:**
- `d`, `display` and `show` as **aliases** for the same ASCII board renderer
  (rank/file labels and borders from `+ - |`), including FEN, side to move, castling
  and recorded EP metadata. No graphical dependency.
- `eval` and `help`; plan useful related extensions (`legal moves`, `perft`/`divide`,
  `bench`, compiler/build info) as their backends land. **Generate `help` from the
  authoritative command/option registry** so it stays accurate.
- These are **UCI-loop extensions, not standard UCI commands.** In UCI mode, emit
  requested human-readable output as **properly serialized `info` lines** (including
  each board line); reserve unsolicited diagnostics for stderr. No banners, prompts
  or ANSI escapes in protocol output.
- Commands inspect a **safe owned/published snapshot** of the user-set position — not
  a live worker's changing `Position` or a dangling copied history chain. Document
  busy behavior; no races or silent mutation of position/search. `eval` identifies
  evaluator, score units and perspective. **Until NNUE exists, explicitly label the
  temporary Material evaluation.**
- Reference behavior was inspected in Gargantua at
  `5ae52eeb8212be55c550e0ba2cf7da03208af220` (`src/uci.cpp` `d`/`eval`/`help`;
  `src/position.cpp` bordered board). Use it as behavioral inspiration, **not**
  permission to copy its global-state/concurrency design.

**Acceptance evidence:** transcript checks for each alias; `help` matches the
registry; `eval` labels the evaluator; no protocol pollution; snapshot safety.
**Status: planned.**

## SEARCH-1 — configurable workers and caches  *(planned)*

**Milestone:** M2 (single worker + real configurable TT); M7 (multi-worker scaling).
**Requirement:**
- **`Threads`** (actual search-worker count), **`Hash`** (TT memory), **`Clear Hash`**
  (a button) with documented units, validated bounds and allocation-failure behavior.
  The **reader thread is separate from the advertised search-worker count.** Early
  single-worker milestones must not advertise nonfunctional multi-worker settings.
- Preallocated per-worker state + an efficient shared TT with a **defined race-free
  C++ memory model**, collision/replacement rules, generation handling and
  mate-score semantics. **"Lockless" does not permit non-atomic data races.**
  Threads/hash/network changes must synchronize with worker lifetime; never
  resize/free storage in active use.
- Measure scaling on **real cores**, including SMT and heterogeneous-CPU effects,
  instead of assuming every logical CPU improves performance. Add race/stress
  verification when concurrency lands. Keep internal caches private unless an
  additional user setting has a clear purpose.

**Acceptance evidence:** validated bounds + allocation-failure behavior; the memory
model is explicit; a race/stress pass is clean; scaling measured on real cores.
**Status: planned.**

## TB-1 — Syzygy integration  *(planned)*

**Milestone:** M7 (advanced UCI capabilities).
**Requirement:**
- **`SyzygyPath`**, plus appropriate probing controls (`SyzygyProbeLimit`,
  `SyzygyProbeDepth`, `Syzygy50MoveRule`) **when backed by real functionality.**
  Handle paths with spaces, OS-appropriate directory-list separators, empty/changed
  paths and missing files. Do not copy another engine's advertised limits without
  implementation support.
- A **pinned, attributed probing implementation** with a documented thread-safety /
  lifetime contract. Integrate **WDL/DTZ** and the fifty-move rule correctly into
  root selection / search / TT scoring; **distinguish tablebase results from
  checkmate and from NNUE values.** Report genuine `tbhits`.
- Missing/ineligible probes must fall back to normal search; errors must **not**
  masquerade as draws or wins. Verify known endgames and probe eligibility against a
  trusted reference.

**Acceptance evidence:** known endgames verified against a trusted reference; genuine
`tbhits`; clean fallback on missing/ineligible probes; no error-as-draw/win.
**Status: planned.**

---

### Primary references (recheck capabilities when implementing each task)
- UCI specification: https://backscattering.de/chess/uci/
- Stockfish commands/options: https://official-stockfish.github.io/docs/stockfish-wiki/UCI-Protocol-and-Stockfish-Commands.html
- Stockfish build guidance: https://official-stockfish.github.io/docs/stockfish-wiki/Compiling-from-source.html
- Clang cross-compilation: https://clang.llvm.org/docs/CrossCompilation.html
- GCC optimization options: https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html
- C++ language support: https://gcc.gnu.org/projects/cxx-status.html , https://clang.llvm.org/cxx_status.html
- Standard-library support: https://gcc.gnu.org/onlinedocs/libstdc++/manual/status.html , https://libcxx.llvm.org/Status/Cxx26.html
- User's command reference (Gargantua): https://github.com/mkd/gargantua
