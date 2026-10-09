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
| `StateInfo`/undo stack, `MoveDelta`s | mutable | per-worker | per worker (≤ MAX_PLY_STACK for the search stack) | n/a |
| `StateInfo.prev` chain + `Position.history` | mutable | per-worker | **uncapped** (caller-owned, not `MAX_PLY`-bounded) | n/a |
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
/* One board-square change: the square and its piece before and after the move.
 * NO_PIECE in a field means that square was / is empty. */
struct SquareEdit { Square sq; Piece before; Piece after; };

/* The reversible board change of one move: the applied Move + the populated
 * edits. A null move has zero edits; a quiet move 2; an EP capture 3; an orthodox
 * castle 4 (MAX_EDIT = 4). `edits` is the full chess-level before/after that a
 * future NNUE consumer rebuilds its feature deltas from. */
struct MoveDelta { Move move; int edit_count; SquareEdit edits[4]; };

/* The undo record: the board delta + the previous (irreversible) scalars + a
 * link into the repetition-history chain. `prev` points at the earlier record
 * (NULL for the first record after a setup). A record's null-ness is its
 * `move == 0` / zero edits; no extra flag is stored. The caller owns the records
 * (stable addresses) and undoes in strict LIFO order. */
struct StateInfo {
  MoveDelta  delta;
  u64        prev_key;      u8 prev_side;  u8 prev_cr;
  Square     prev_ep;       u16 prev_halfmove;  u16 prev_fullmove;
  struct StateInfo *prev;
};
```

- **make()** (`pos_make_move`): apply `move` to `Position` (mailbox + derived
  bitboards + side + cr + ep + rule50 + the exact key), recording the bounded
  `MoveDelta` edits + the previous scalars in the caller-supplied `StateInfo`.
  Validates every input before the first write; on failure it leaves both the
  `Position` and the record byte-for-byte unchanged.
- **unmake()** (`pos_unmake_move`): restore **all** logical state (revert the
  edits; restore the scalars; the exact key; the history head) from the record;
  recompute nothing by guessing — every restored field matches a from-scratch
  recompute. The record is not modified and may be reused.
- **En passant:** `Position.ep_sq` retains the recorded target in every setup and
  move path. The FEN loader preserves it, a double pawn push records its passed
  square even without a legal captor, and other moves clear it. Unmake restores
  the exact previous record. Only the **key** canonicalizes: `pos_canon_ep_file`
  contributes the target file iff at least one EP capture is king-safe. Do not
  erase an uncapturable raw record in make/unmake. This is QwenChess's storage
  convention, independent of Stockfish's internal board representation.
  **Null moves** clear the recorded EP while applied and restore it on undo.
- The core `make_move(...)` function already constructs an encoded Move. Board
  mutation uses distinct names: `pos_make_move` / `pos_unmake_move` for real
  moves and **`pos_make_null_move`** for a pass. A null is a distinct entry point
  because the ordinary `pos_make_move` rejects the move value 0 (the no-move
  sentinel, from=to=0 with no flags), so a pass can never be spelled accidentally.
  A null: zero board edits, flip the side, clear the recorded EP, and **preserve
  both counters** (halfmove + fullmove) for either color (QwenChess counter
  policy — a synthetic null neither advances nor resets them; a full-width 65535
  clock simply round-trips). Its key updates exactly (toggle the side; remove the
  old canonical EP contribution). It is rejected (leaving both outputs unchanged)
  when the side to move is in check — a pass cannot address a check.
- Undo comparisons inspect logical fields and derived caches, not struct padding.
  T004 uses hand-built moves; seeded legal sequences are a T005 integration gate.
- **Repetition history:** every applied record (a real move or a null) links to
  its predecessor via `StateInfo.prev`; `Position.history` points at the latest
  applied record (NULL on a fresh setup). The chain is **caller-owned at stable
  addresses** and is **NOT capped at `MAX_PLY`** — the pre-root game history and a
  bounded search undo array may live in separate storage. `Position` holds only a
  non-owning pointer (it frees nothing); `pos_rebuild` leaves it untouched, a
  successful FEN/reset setup sets it to NULL (abandoning the chain), and a failed
  load preserves it. **`pos_repetition_count`** is the production occurrence
  query: it counts the current key's occurrences in the reachable history plus the
  current node if real, and a **null node counts 0** and acts as a walk boundary
  (the walk never crosses a null transition into earlier history). It is an
  occurrence query only — threefold/fivefold/rule-50 draw adjudication is a search
  policy, not part of this contract.

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
