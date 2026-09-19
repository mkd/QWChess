# QwenChess — State

**Milestone:** M0 (scaffold) — complete. **Next up:** M1 board correctness.
**Last verified commit:** the T001 scaffold commit (this task). *(A git identity
was not configured on this host, so the commit is staged but not yet authored;
see "Blockers". The working tree is fully validated.)*

## Completed this session (T001)
- Project established: single `Makefile` (GCC default, `CC=clang` optional,
  separate `build/{release,debug,sanitize}/`), `src/core/types.h`,
  `src/platform/clock.{h,c}`, `src/engine/main.c` (self-IDs as scaffold, not a
  playable engine), `tests/scaffold_test.c`, `tools/net_{fetch,test}.sh`, root
  `AGENTS.md`/`README.md`/`.gitignore`/`COPYING`/`PROVENANCE.md`, `docs/*`.
- Corrected architecture persisted: per-worker mutable Finny/accumulator caches
  (only weights + lookup tables shared), chess-level `MoveDelta` (no NNUE types in
  Position/StateInfo), `MAX_PLY=256` + `int32_t Value` + wide counters, responsive
  UCI (reader thread + atomic stop; multi-worker deferred).

## Latest checks (all run this session)
- `make` / `make debug` / `make sanitize` — build clean, **no warnings**.
- `make test` — PASS (exit 0). `make test-sanitize` — PASS under ASan/UBSan (exit 0).
- `make net` — fetched `nn-1a298aa575a0.nnue` from `tests.stockfishchess.org`,
  verified SHA-256 `1a298aa575a085434d29027978dc36867fe9c5bcea9376654b7a8eba1e52dfc2`
  (98,511,183 bytes), installed to `networks/` (gitignored). Idempotent re-run OK.
- `make test-net` — bogus net rejected, wrong-prefix rejected, real net verified.
  Explicit corrupted-copy check exits 4; genuine net byte-identical before/after.
- Pinned reference re-verified: tag `sf_19` → commit `edb0d9db…`; NNUE constants
  re-fetched from source (see `docs/NNUE_COMPATIBILITY.md`).

## Blockers / open items
- **Git identity not configured** (no local/global/system/env `user.name`+email).
  The T001 commit is staged but not authored — set an identity, then `git commit`.
- Clang not installed (GCC-only for now; fine). Match runner (cutechess/fastchess)
  not installed (needed at M5, not now). `reference/` Stockfish checkout not made
  (needed at M3 oracle task; procedure in PROVENANCE.md).
- `docs/NNUE_COMPATIBILITY.md` §10 items are **○ pending** source inspection.

## Exactly one next task
**T002 — core primitives** (bitboards, move encode/decode, Zobrist, **portable**
slider attacks + slow reference; **no magic/PEXT**). Deps: T001. See PLAN.md.
