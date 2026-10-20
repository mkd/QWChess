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
  histories. Depends on core + position. Two non-castling generators now exist: the
  **pseudo-legal** (cp1, done) is **stateless and read-only** over `Position` (writes
  candidates into a caller buffer; no make/unmake, no king-safety query); the **legal**
  (cp2, done) filters those candidates for own-king safety by **probing a scratch copy
  of the Position** (one bounded stack-local per call) via `pos_make_move` /
  `pos_is_in_check` / `pos_unmake_move` — the caller's Position and history are left
  untouched, and it owns no mutable state of its own (the lifted per-candidate make/
  unmake ban applies to this layer only). Both exclude castling (cp3) and null moves;
  SEE + evasions + staged ordering are later (M2/M5).
  **Current:** `movegen_pseudo_noncastling(const Position&, std::span<Move>,
  std::size_t&)` (cp1 primary) + a documented pointer adapter, and
  `movegen_legal_noncastling(const Position&, std::span<Move>, std::size_t&)` (cp2,
  single span form, no pointer overload) — the latter keeping a candidate iff the
  original mover's king is unattacked on the resulting board (geometric attacks incl.
  pinned enemy pieces; all four promotions, EP evasions, blocks and sacrifices).
- **nnue** — exact loader, feature extraction (a *consumer* of Position), scalar
  reference, incremental accumulators, optimized kernels, the eval wrapper.
  Depends on core + position. **Never owned by Position; never owns a board.**
- **search** — iterative deepening, alpha-beta/PVS, quiescence, stack, TT,
  limits, time management, heuristics. Depends on core+position+movegen+nnue.
- **engine** — UCI protocol, options, search lifecycle, worker ownership,
  cancellation, serialized output, `main`. Depends on the rest.

Planned-but-not-yet-created modules (documented here; do not scaffold dozens of
empty files): `position/` internals beyond the scaffold, the `nnue/` sub-files,
`search/`, and the responsive-UCI pieces of `engine/`. (`movegen/` now exists:
T005 cp1's non-castling pseudo-legal + cp2's non-castling legal generators, in
`movegen.{hpp,cpp}`.)

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

## Foundational types (frozen in `src/core/types.hpp`)

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

```cpp
/* One board-square change: the square and its piece before and after the move.
 * NO_PIECE in a field means that square was / is empty. */
struct SquareEdit { Square sq; Piece before; Piece after; };

/* The reversible board change of one move: the applied Move + the populated
 * edits. A null move has zero edits; a quiet move 2; an EP capture 3; an orthodox
 * castle 4 (MAX_EDIT = 4). `edits` (a std::array since CPP20-2B) is the full
 * chess-level before/after that a future NNUE consumer rebuilds its feature
 * deltas from. There are no separate squares/pieces/occ/king-sq-delta members. */
struct MoveDelta { Move move; int edit_count; std::array<SquareEdit, MAX_EDIT> edits; };

/* The undo record: the board delta + the previous (irreversible) scalars + a
 * link into the repetition-history chain. `prev` points at the earlier record
 * (nullptr for the first record after a setup). A record's null-ness is its
 * `move == 0` / zero edits; no extra flag is stored. The caller owns the records
 * (stable addresses) and undoes in strict LIFO order. */
struct StateInfo {
  MoveDelta  delta;
  u64        prev_key;      Color prev_side;  u8 prev_cr;
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

## Invariants & C++20 safety

- `Position` bitboards/occupancy must equal a from-scratch recompute after every
  make/unmake (checked in tests).
- C++20 memory model: **racy non-atomic access and `volatile` are NOT
  synchronization.** The TT and any later worker sharing use C++ atomics
  (`std::atomic`) with a defined memory order; keep a **deterministic single-worker**
  mode for debugging. The shipped logic is C++20 and may use the standard library at
  the implementation level; the exceptions/RTTI policy, naming and documentation
  conventions are set in the **Engineering policy** section below (this checkpoint
  introduces neither exceptions nor RTTI), and there is no reliance on implicit
  narrowing/UB.
- No heap allocation on the search/eval hot path; preallocate per worker. Cold
  protocol/diagnostics code stays off the hot path.
- NNUE integer pipeline follows SF19 exactly (types/order/clipping). Handle signed
  shifts, overflow, aliasing and narrowing **explicitly** — no reliance on UB.
- Alignment is chosen from the supported kernels/hardware (not a single hard-coded
  cache-line size everywhere).

## Engineering policy (C++ conventions)

The detailed engineering policy. `AGENTS.md` carries a concise version and points
here; this is the reference.

- **Language direction.** Clean, readable, **portable** C++ with measured performance
  and a parallel-search architecture. **C++20 is the current build baseline; C++26 is
  deferred** pending an explicit user request (no C++26 migration/readiness work is
  scheduled; if later requested, REQUIREMENTS.md BUILD-1 owns the C++26 readiness
  review — a flag being accepted is *not* proof the feature works). Use templates,
  `constexpr` specialization and standard-library facilities
  **when they improve clarity or performance**; prefer the simpler portable
  implementation when performance is indistinguishable. Focused C-style code /
  intrinsics, or an isolated C component, remain allowed when appropriate and
  justified. **Do not claim a language feature is faster merely because it is newer.**
- **Exceptions / RTTI.** The earlier *blanket* prohibition on exceptions/RTTI and the
  *restrictive* standard-header list are **removed.** This checkpoint introduces
  neither; existing hot-path operations keep **explicit non-throwing** failure
  behavior (return codes / out-params) and there is **no per-node heap allocation** or
  speculative disabling flags. Exceptions/RTTI may be introduced later only where
  justified and measured.
- **Naming.** PascalCase for domain types (`Position`, `StateInfo`, `Move`);
  snake_case for functions/methods/variables/namespaces (`pos_make_move`, `qwc`);
  UPPER_SNAKE_CASE for named constants and enumerators (`NO_SQUARE`, `MV_CASTLE`). All
  domain types and engine structs live in the project **`qwc` namespace** (applied in
  CPP20-2B); tests opt in with `using namespace qwc` while production code keeps full
  qualification. Representation aliases (`u8`, `u32`, `Value`) remain. The remaining
  member-prefix and human-facing output cleanup is coordinated in CPP20-3. Classes,
  wrappers and templates **must serve a purpose.**
- **Documentation.** Document every production header, type, function/method (including
  private/inline helpers) and named constant: purpose, units, ranges/sentinels,
  invariants and, where relevant, preconditions, errors, mutation, lifetimes,
  ownership, concurrency and allocation constraints. Short helpers may need one
  sentence. Keep public contracts **beside declarations** and implementation reasoning
  **beside definitions.** Explain tricky chess rules and optimization assumptions;
  avoid boilerplate or unsupported promises. Track remaining coverage by module.
- **Copying / I/O.** Prefer **typed copying** and clear standard interfaces.
  `std::memcpy` remains valid for appropriate byte / trivially-copyable operations; do
  not apply it indiscriminately to future owning types. Review human-facing output
  coherently in CPP20-3 (`std::cout` is a reasonable default; replacing `std::printf`
  alone proves no speed gain).
- **Fixed-size storage.** Fixed-size *data* arrays are `std::array` (position
  pieces/occupancy, the `MoveDelta` square edits, attack/zobrist/fen tables, per-test structs) — over C
  arrays for a stable layout, value semantics, `.data()`/`.size()` and sized iteration,
  with no heap. A C-string / move buffer handed to a `char*`/`Move*` boundary takes
  `.data()`/`.size()`. Prefer **typed** initialization (`fill`, aggregate value-init) and
  **typed** equality (`==` / element compare) over `sizeof`-sized `memset`/`memcmp` byte
  ops; retain a raw `char out[N]`-style *parameter* where a C-string/move buffer is the
  natural stable interface, and keep intentional byte-handling of trivially-copyable data
  (e.g. a deliberate object-representation corruption test) byte-based. (Applied in
  CPP20-2B; `sizeof(T)`/`memcpy` survive only for legitimate byte handling.)
- **Ownership of types.** Preserve **one authoritative `Position`**, chess-level
  undo/deltas, and **exclusive worker ownership** of mutable board/search/NNUE caches
  (see the ownership table above). Shared tables/weights are **immutable after
  initialization**; shared mutable TT/state needs an explicit, valid C++
  synchronization + lifetime design.

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

One former later-only item is now **active** (the C++20 migration); the other remains
deferred until measured need. See PLAN.md for the full CPP20 workstream.

- **C++20 migration — now active.** All *shipped* engine logic is C++20 (see
  AGENTS.md); the C++ standard library is permitted at the implementation level.
  **CPP20-1** (language/build foundation), **CPP20-2A** (scoped enums + closeout) and
  **CPP20-2B** (namespace/naming/typed constants/`std::array` storage + bitboards +
  build matrix) are done; **CPP20-3** (interfaces/ownership + platform services +
  output + docs) and **CPP20-4** (final audit + build profiles + same-ISA comparison +
  C++26 readiness (deferred) + cross-build matrix) remain. Continuing rules: gate on C++20 + the platform standard library, keep the
  full correctness/NNUE/search regression suite green, record the new build +
  sanitizer coverage, keep UCI, determinism and the single-worker mode byte-identical,
  and keep `Position`/`StateInfo`/`MoveDelta` plain aggregates with no NNUE types.
  Note: C++20 *source* portability does not by itself establish runtime compatibility
  on every CPU/OS. See PLAN.md for the full workstream and the Engineering policy above.
- **Targeted platform/performance tuning.** Only after a measured bottleneck on the
  actual execution CPU: hot-path cache alignment/branch layout, vector width and
  instruction-set selection, LTO/PGO, and a benchmark-driven choice between the
  standard C library and a purpose-built allocator. No speculative optimization or
  unmeasured micro-tuning. (NNUE SIMD kernels are the dedicated M6 scope; this item
  covers the general platform layer.)
