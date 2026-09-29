# QwenChess — State

**Milestone:** M1 (board correctness). **T003 (Position/FEN/Zobrist) complete** —
board core + strict FEN load/emit, the EP regression repair, and the source-audit
boundary corrections, all applied and verified on this Debian machine, committed in
this checkpoint. M1 stays open (T004 make/unmake, T005 movegen+perft).
**Baseline (last verified) commit:** `2939e22` (T003: Position board core + strict
FEN load/emit). This checkpoint commits the EP repair + source-audit corrections on
top of it. Git identity `mkd`/`claudiomkd@gmail.com`.

## Prior local EP regression session (reported)
- **Reported root cause:** the engine's EP logic was *correct*.
  The defect was (a) the on-disk `tests/fen_test.c` fixtures had been **altered** —
  5 of the 8 differed from the task's originals — and (b) the handoff **misdescribed**
  the contract as hashing "the captor's file". In fact the loader retains the recorded
  EP square; only the **key** canonicalizes, and `pos_canon_ep_file` returns the **target
  square's** file (d for a d6 record) iff a capture is king-safe, else −1.
- **`src/position/position.{h,c}`:** added `pos_ep_capture_is_safe(pos, captor)` — a
  public per-captor form of the existing king-safety test (the captor must be a
  friendly pawn on the captured pawn's rank, on an adjacent file; the king does not
  move; the removed captured pawn no longer attacks). `pos_ep_is_legal` now shares that
  derivation. **Behavior-preserving** (standalone probe: 0/8 regressions before/after).
- **`tests/fen_test.c`:** the regression now uses the **8 exact task originals** (all
  valid, all must load) with **independent placement lists**, asserting: placement,
  recorded EP retained, canonical file, legality, **specific captors** (e5 / c5-only /
  none), the key XOR-contract, **rank-reflect + color-swap mirrors** (black to move),
  and emit→reload preserving the recorded EP + logical state. All active under NDEBUG.

## The eight fixtures (verified engine behavior, all load)
#1/#4/#5 legal (canon d=3; #5 = "c5d6 only"); #2 (pinned), #3 (rank-exposed),
#6 (no captor), #7 (bishop diagonal), #8 (king still in check) uncapturable
(canon −1). Recorded EP kept in every case; the key uses the target-file key iff legal.

## Checks reported by the prior local session
- `make check-c17` → **13 files OK**, no non-C17 constructs.
- `make test` (release) → all 7 binaries pass; fen: roundtrip 15, reject 63,
  **ep 62, ep.mirror 48, ep.roundtrip 40**, emit.bounds 7 → **0 fail**.
- `make test-sanitize` → all **PASS** under ASan/UBSan, no errors/leaks.

## Source audit of qwc_code.zip (2026-09-28)
- The uploaded EP implementation removes the correct captured pawn, tests the
  capturing king, and never calls pos_rebuild from its scratch simulation. The
  source excerpt in the previous handoff did not match the actual file. The eight
  exact EP fixtures and expectations are unchanged by the audit patch.
- Fixed a structural halfmove cap of 100: FEN now accepts the full u16 storage
  range, independent of draw policy. Overflow is rejected before narrowing.
- Fixed undefined piece-code acceptance, bounded rebuild/hash table indexing for
  malformed mailbox values, checked the empty-piece cache, and made FEN emission
  fail without writing when the Position is invalid.
- Both EP queries now reject malformed EP metadata, including a missing victim.
- Aligned the make/unmake docs: preserve recorded EP, canonicalize only the key,
  use full-width undo counters, and avoid core's existing make_move name.
- Direct checks on the patched source: strict C17 over **14 files**; **8 release
  test binaries**; **8 ASan/UBSan test binaries**; release/debug/sanitize engine
  builds and release scaffold smoke. All passed. The new boundary tests perform
  **1,344 checks**, also passing under NDEBUG. No compiler warnings observed.
- This review environment cannot run LeakSanitizer (cannot inspect /proc task
  state). The sanitizer pass used ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 and
  UBSAN_OPTIONS=halt_on_error=1; leak checking was NOT verified here. Run the
  normal make test-sanitize on Debian before publishing.
- The new regressions against the original uploaded source reproduce a UBSan
  out-of-bounds access at position.c:190 (piece-table index 16). A standalone
  probe also reproduces the halfmove cap, unused codes 7/8/15 being accepted, and
  per-captor success without the captured pawn. The patch resolves these cases.
- The ZIP contains no Git history or network tooling; this audit did not verify
  historical commits, remotes, network download/redistribution, NNUE or gameplay.

## Independent verification on this machine (Debian, GCC 16.2.0, post-audit-apply)
All four representation-boundary defects the audit named are re-verified fixed. The
patch applied cleanly (`git apply --check` then apply, no `--index`); all 14 touched
files match the audited `after_sha256`. The eight EP fixtures and expectations are
byte-identical to the audited table and pass.
- `make check-c17` → **14 files OK** (was 13; + `position_boundary_test.c`).
- `make test` (release) → all **8** binaries pass. `position_boundary_test` =
  counters 72 + piece_codes 1216 + ep_metadata 51 + emit_capacity 5 = **1,344
  checks**, 0 fail (matches the audit's count). fen: ep 62 / ep.mirror 48 /
  ep.roundtrip 40 (the 8 originals, unchanged).
- `make test-sanitize` (**normal** target, no `ASAN_OPTIONS` override) → all **8**
  pass under ASan+UBSan, 0 failures. AddressSanitizer + UBSan instrumentation is
  confirmed **active and working** (a deliberate heap-OOB probe is caught, exit 1).
  LeakSanitizer runs ("checking for leaks") but is environmentally silent in this
  sandbox — a deliberate reachable heap leak is not reported even with
  `detect_leaks=1` (the `SuspendAllThreads` step reports from a child pid), while
  `/proc/self/{maps,task}` are readable. This is **moot** for the shipped code:
  neither `src/` nor `tests/` heap-allocates (zero `malloc`/`free` by design), so
  there is nothing that can leak. No Makefile leak workaround was added.
- `make release debug sanitize` → all engine builds exit 0.

## Blockers / open items
- `reference/` Stockfish checkout not made (M3 oracle); match runner not installed
  (M5). Nothing else blocks T004.

## Exactly one next task
**T004 — make/unmake + `StateInfo`/undo** (`src/position/state.{h,c}` +
`pos_make_move`/`pos_unmake_move`): one authoritative `Position`, reversible `StateInfo`
stack; make then unmake restores **all logical fields** (pieces, occupancy, side,
castling rights, *recorded* EP, halfmove, fullmove, key) plus the derived caches
exactly. Corrections to the earlier note: the halfmove counter resets on **a pawn
move OR a capture** (not "move+capture"); the restore check compares **logical fields
+ key + derived caches by field** (not a blind struct `memcmp`, since the derived
caches are recomputed separately); and **seeded legal-move sequences require T005
movegen**, so T004's round-trip tests must use hand-built `Move` values until
movegen exists. Deps: T003 (done).
