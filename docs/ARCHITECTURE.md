# QwenChess Architecture

Modularity supports local reasoning and fast compiled code: concrete structs,
direct calls, a small dependency DAG. No generic engine framework, no plugin
system, no function-pointer dispatch inside nodes. Conventions are frozen here;
speculative abstractions are deferred.

## Module dependency map (a strict DAG — no cycles)

```
platform  ←  core  ←  position  ←  movegen  ─┐
   ▲            ▲           ▲                ├─→ search  →  engine (uci/main)
   └────────────┴───────────┴──→ nnue ──────┘        ▲
        (nnue CONSUMES position; one-way)            │
                                                     └─ (search calls nnue evaluate)
```

- **platform** — allocation/alignment, monotonic clock, CPU feature detect,
  atomics wrapper, build/test/bench helpers. No dependencies.
- **core** — fixed-width types, `Square`/`Piece`/`Move` encoding, 64-bit bitboards,
  attack generation, Zobrist hashing. Depends on platform.
- **position** — the one authoritative board: `Position`, FEN, legal state,
  make/unmake, repetition history, `StateInfo`/`MoveDelta`. Depends on core.
- **movegen** — legal/pseudo moves, captures, evasions, SEE, staged ordering,
  histories. Depends on core + position.
- **nnue** — exact loader, feature extraction (a *consumer* of Position), scalar
  reference, incremental accumulators, optimized kernels, the eval wrapper.
  Depends on core + position. **Never owned by Position; never owns a board.**
- **search** — iterative deepening, alpha-beta/PVS, quiescence, stack, TT,
  limits, time management, heuristics. Depends on core+position+movegen+nnue.
- **engine** — UCI protocol, options, search lifecycle, worker ownership,
  cancellation, serialized output, `main`. Depends on the rest.

Planned-but-not-yet-created modules (documented here; do not scaffold dozens of
empty files): `position/` internals beyond the scaffold, `movegen/`, the `nnue/`
sub-files, `search/`, and the responsive-UCI pieces of `engine/`.

## Ownership & lifetimes (CORRECTED)

| Artifact | Mutability | Sharing | Lifetime | Reset on network change? |
|----------|-----------|---------|----------|--------------------------|
| Network weights (featureTransformer + 8 layer stacks) | immutable | **shared read-only** | process | reloaded |
| Immutable lookup tables (attacks/magics, Zobrist) | immutable | shared read-only | process | n/a |
| **Finny / accumulator refresh cache** | **mutable** | **per-worker** | per worker | **yes (rebuild)** |
| `AccumulatorStack` (per-ply accumulators) | mutable | per-worker | per worker | yes |
| `Position` | mutable | **per-worker** (single board) | per worker | n/a |
| `StateInfo`/undo stack, `MoveDelta`s | mutable | per-worker | per worker (≤ MAX_PLY_STACK) | n/a |
| `SearchStack`, histories | mutable | per-worker | per worker | n/a |
| `TranspositionTable` | mutable | shared (atomics-guarded) | process (sized by option) | cleared |

`Position` is the **authoritative** chess state. Derived data (per-piece
bitboards, a cached piece snapshot inside a Finny entry) is permitted and must
always be reproducible from `Position`. **Only network weights and immutable
lookup tables are shared read-only.** Every mutable accelerator — including the
Finny/accumulator refresh cache — is per-worker and is rebuilt/invalidated on a
network change.

### Memory (per worker, at `MAX_PLY=256`)
- `AccumulatorStack`: `(MAX_PLY+1)` states ≈ 257 × ~4.2 KiB ≈ **~1.1 MiB**.
- Finny refresh cache: `64` king-squares × `2` perspectives ≈ **~269 KiB** (per worker).
- `Position` + `StateInfo` stack + histories: a few KiB.
- Network weights (shared, single copy): **~110 MiB** in memory.
So a 14-thread run ≈ 14 × ~1.4 MiB + 110 MiB shared ≈ **~130 MiB** — well within
the host's RAM. (The on-disk net is 94 MiB; weights decompress larger in memory.)

## Foundational types (frozen in `src/core/types.h`)

- `Square` = 0..63 (`a1=0 … h8=63`, `s = rank*8+file`); `NO_SQUARE = 64` sentinel.
- `Piece`/`PieceType`/`Color` values mirror Stockfish to ease later NNUE parity.
- `Move` = 32-bit `{from:6, to:6, flags:3, reserved:1, promoType:3}` (bits 0..18;
  bits 19..31 reserved, must be 0 per `move_repr_is_valid`); `0` = null/no move.
  Flags: `MV_EP|MV_PROMO|MV_CASTLE` (bits 12..14); bit 15 reserved; `promoType`
  2..5 = KNIGHT..QUEEN under a `MV_PROMO` move.
- `Value` = `int32_t`, **side-to-move** perspective; the one score type.
- `MAX_PLY = 256` (QwenChess's own; SF's 246 is an upstream reference fact only).
- `MAX_PLY_STACK = MAX_PLY + 1`.
- Counters are `u16` and **distinct**: `SearchPly` (root-relative) ≠ game history
  ≠ `FullMoveNumber` (FEN). `HalfMoveClock` is wide (not `u8`) and validated.
  FEN accepts halfmove 0..65535 and fullmove 1..65535. Draw thresholds are
  adjudication rules, not structural FEN limits. Reject input overflow before
  narrowing; T004 must check increments before they overflow.

### Score scale (derived from `MAX_PLY=256`; ordering asserted at compile time)
```
mated_in_max(-30744) < tb_loss(-30487) < draw(0) < tb_win(30487) < mate_in_max(30744) < infinite(32000) < none(32001)
```
Mate distance: `mate_in(ply)=31000-ply`. NNUE static eval is clamped to the open
interval `(tb_loss, tb_win)` so it never collides with mate/TB scores. The TT
stores **ply-normalized** mate scores and must treat rule50/repetition-dependent
entries as non-reusable across search cycles.

## The reversible-move contract (chess-level)

`Position` and `StateInfo`/`MoveDelta` are **chess-level only** — they must not
contain NNUE/SFNNv16 types or compute feature indices.

```c
/* Chess-level description of a reversible board change (before -> after). */
struct MoveDelta {
  Move      move;          /* from, to, flags (ep/promo/castle) */
  Piece     captured;      /* piece removed from `to`, or NO_PIECE (ep handled via flags) */
  Piece     moved;         /* the moving piece (pre-move identity) */
  uint8_t   prev_cr;       /* castling rights before the move */
  HalfMoveClock prev_rule50; /* full-width halfmove clock before */
  FullMoveNumber prev_fullmove; /* fullmove number before */
  u64       prev_key;      /* position key before */
  Square    prev_ep;       /* exact recorded ep square before, or NO_SQUARE */
  Color     prev_side;     /* side to move before */
};
```

- **make():** apply `move` to `Position` (mailbox + derived bitboards + side +
  cr + ep + rule50 + hash), record a `MoveDelta` of the previous irreversible
  state, push a new `StateInfo`.
- **unmake():** restore **all** logical state from the `MoveDelta` + `move`;
  recompute nothing by guessing — every restored field matches a from-scratch
  recompute.
- **En passant:** `Position.ep_sq` retains the recorded target in every setup and
  move path. The FEN loader preserves it, a double pawn push records its passed
  square even without a legal captor, and other moves clear it. Unmake restores
  the exact previous record. Only the **key** canonicalizes: `pos_canon_ep_file`
  contributes the target file iff at least one EP capture is king-safe. Do not
  erase an uncapturable raw record in make/unmake. This is QwenChess's storage
  convention, independent of Stockfish's internal board representation.
  **Null moves** clear EP while applied, restore it on undo, and add no fictitious
  repetition.
- The core `make_move(...)` function already constructs an encoded Move. T004
  board mutation uses distinct names such as `pos_make_move` / `pos_unmake_move`.
- Undo comparisons inspect logical fields and derived caches, not struct padding.
  T004 uses hand-built moves; seeded legal sequences are a T005 integration gate.
- **Repetition history** (a chain of pre-move states) is available independently
  of the search stack, so pre-root repetitions remain detectable.

## The NNUE boundary (consumer of Position)

- `nnue` derives **feature indices** from `Position` (king-dependent transform,
  threat rays, pawn pairs) and owns its **pending updates** (dirties) + accumulators.
- To update incrementally, the NNUE layer reads the chess-level `MoveDelta`
  (moved piece, captured piece, king squares, side) plus current `Position` and
  computes the per-side feature-index delta itself. **`MoveDelta` is sufficient
  chess information** (moved/captured/king/side); the *NNUE* computes which
  feature indices that implies — including **distant slider threat-ray changes**
  (a slider move can re-target attacked squares far from the move) and
  **pawn-pair changes** (captures/moves that add or remove a pair). Documenting the
  exact bit-math for those is pending source inspection (see
  `NNUE_COMPATIBILITY.md`).
- **No circular dependency:** Position → (nothing NNUE). NNUE → Position (reads).
- **Initial implementation = scalar full refresh** (correct, simple). Lazy updates,
  refresh caches and incremental dirties come only **after** scalar parity with the
  oracle. Do not build the incremental machinery before reference parity.

## Invariants & C-safety

- `Position` bitboards/occupancy must equal a from-scratch recompute after every
  make/unmake (checked in tests).
- C11/C17 memory model: **racy non-atomic access and `volatile` are NOT
  synchronization.** The TT and any later worker sharing use C11 atomics with a
  defined memory order; keep a **deterministic single-worker** mode for debugging.
- No heap allocation on the search/eval hot path; preallocate per worker. Cold
  protocol/diagnostics code stays off the hot path.
- NNUE integer pipeline follows SF19 exactly (types/order/clipping). Handle signed
  shifts, overflow, aliasing and narrowing **explicitly** — no reliance on UB.
- Alignment is chosen from the supported kernels/hardware (not a single hard-coded
  cache-line size everywhere).

## Responsive UCI (required from the first UCI milestone)

- A **command-reader thread** is independent of the (single) **search worker**, so
  `stop`/`quit`/option changes are honored promptly even mid-search.
- Cancellation is **atomic** (a stop flag the worker polls between nodes/iterations).
- **Multiple search workers are deferred**; the responsive control path is not.
- Protocol output is **serialized to stdout**; other diagnostics go elsewhere.
- Options/hash/network changes are applied safely **between** searches.

## Deferred (deliberately not built yet)

Magic/PEXT attacks (start portable slider generation), SIMD kernels, LTO/PGO,
cache layout, SMP scaling, Chess960, pondering, MultiPV, Syzygy, NNUE training,
GPU inference, distributed search. Search strength heuristics (SEE, LMR, NMP,
futility, …) land one at a time after a stable baseline.
