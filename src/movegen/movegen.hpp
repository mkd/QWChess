/* QwenChess: non-castling pseudo-legal move generation.
 *
 * Generates every pseudo-legal candidate move EXCEPT castling (orthodox castling
 * and own-king safety arrive in T005 cp2). "Pseudo-legal" here means the piece
 * geometry, occupancy and special-move metadata are correct; own-king safety and
 * resolving check are deliberately NOT filtered at this layer (a king may move to
 * an attacked square and a pinned captor may take).
 *
 * Dependency: movegen -> position/core only (position/core never depend on
 * movegen). It consumes the existing bitboards and attack tables. No new board
 * representation, no NNUE fields, no runtime dispatch framework, no sorting, no
 * SIMD and no magic attacks are introduced.
 *
 * The API is allocation-free and read-only over the Position: it performs no
 * make/unmake, no full validation or rebuild, no king-safety query, and no heap
 * allocation.
 */
#ifndef QWC_MOVEGEN_MOVEGEN_HPP
#define QWC_MOVEGEN_MOVEGEN_HPP

#include <cstddef>

#include "position/position.hpp"

/* Generate every non-castling pseudo-legal candidate of `pos` into `out`.
 *
 * Contract:
 *  - `pos` is an initialized, consistent Position (coherent derived caches) as
 *    produced by the setup/make paths. `out` is caller-supplied, non-overlapping
 *    storage of `capacity` Moves; `count` may be nullptr.
 *  - On success returns 1 and, when `count` is non-nullptr, writes the complete
 *    move count to *count (zero for an empty result).
 *  - On failure returns 0 and, when `count` is non-nullptr, sets *count to zero.
 *    It fails when `pos` or `out` is nullptr, or when the result would exceed
 *    `capacity` (overflow). Invalid pointers are documented here rather than
 *    silently special-cased: a nullptr `pos` or `out` is a programming error.
 *  - Capacity is checked before every append. No Move is written outside the
 *    supplied capacity and a successful result is never truncated. On overflow
 *    an in-bounds partial prefix may remain in `out` but is unusable; no second
 *    generation pass is made to preserve it. With valid pointers a zero
 *    capacity is permitted and succeeds only when the result is empty.
 *  - The Position (board, key, counters and the complete history chain) is left
 *    untouched.
 *  - Enumeration is deterministic and duplicate-free. The order is an internal
 *    convenience and is NOT a public search-ordering contract.
 */
int movegen_pseudo_noncastling(const Position *pos,
                               Move *out, std::size_t capacity,
                               std::size_t *count);

#endif /* QWC_MOVEGEN_MOVEGEN_HPP */
