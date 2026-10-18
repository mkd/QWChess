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
 *
 * There are two overloads of the same generator: the *primary* bounded form (a
 * std::span output buffer + a size_t& count) and a documented *compatibility
 * adapter* (the legacy pointer form). Both share the one generation implementation;
 * the adapter validates its legacy null/signed cases and delegates to the primary.
 */
#ifndef QWC_MOVEGEN_MOVEGEN_HPP
#define QWC_MOVEGEN_MOVEGEN_HPP

#include <cstddef>
#include <span>

#include "position/position.hpp"

namespace qwc {

/* Generate every non-castling pseudo-legal candidate of `pos` into `output`.
 *
 * This is the primary (bounded) form. `output` borrows caller-owned storage of
 * `output.size()` Moves for the duration of the call; `count` is written.
 *
 * Contract:
 *  - `pos` is an initialized, consistent Position (coherent derived caches) as
 *    produced by the setup/make paths; `output` is a nonoverlapping buffer.
 *    `count` is always written (it is a required reference, not an optional
 *    pointer).
 *  - A default-constructed empty span is a valid empty range: the generation
 *    succeeds with `count == 0` only when the position generates zero candidates;
 *    otherwise the first failed append makes the call fail with `count == 0`
 *    (nothing is written to a zero-size span).
 *  - Capacity is checked before every append. No Move is written outside the
 *    supplied capacity and a successful result is never truncated. On overflow an
 *    in-bounds partial prefix may remain in `output` but is unusable; no second
 *    generation pass is made to preserve it. `count` is zero on every failure and
 *    the complete move count on success.
 *  - The Position (board, key, counters and the complete history chain) is left
 *    untouched.
 *  - Enumeration is deterministic and duplicate-free. The order is an internal
 *    convenience and is NOT a public search-ordering contract.
 *
 * Returns true on success, false on overflow. */
[[nodiscard]] bool movegen_pseudo_noncastling(const Position& pos,
                                              std::span<Move> output,
                                              std::size_t& count);

/* Compatibility adapter: the legacy pointer form. `pos` and `out` are
 * caller-supplied (a null `out` fails even with zero capacity; a null `pos`
 * fails); `capacity` is the buffer's size in Moves; `count` is OPTIONAL (it may
 * be nullptr). Returns 1 on success and, when `count` is non-nullptr, writes the
 * complete move count (zero for an empty result); returns 0 on failure and, when
 * `count` is non-nullptr, sets it to zero. The contract is otherwise identical to
 * the primary above. It validates its null pointers before constructing the span
 * and delegates to the same primary generator. */
int movegen_pseudo_noncastling(const Position *pos,
                               Move *out, std::size_t capacity,
                               std::size_t *count);

}  // namespace qwc

#endif /* QWC_MOVEGEN_MOVEGEN_HPP */
