/* QwenChess: perft / divide -- a developer traversal diagnostic.
 *
 * This module sits ABOVE position + movegen (movegen/position never depend on it).
 * It counts legal-move sequences for regression testing. It is NOT a search or UCI
 * implementation: it evaluates nothing and chooses no best move. It reuses the
 * complete `movegen_legal` + the existing reversible make/unmake and performs no
 * evaluation, pruning, transposition caching, null moves, or draw cutoffs.
 *
 * Ownership: a single `Position` is copied once into a diagnostic scratch at the
 * root; the recursion mutates and reverses that scratch in place against a bounded,
 * stable undo stack. No per-node heap allocation and no whole-Position snapshot.
 * The caller's Position and any live repetition-history records it references are
 * left untouched (the scratch's history is detached, so the traversal is
 * independent of the caller's game history and its rule-50/fullmove counters).
 */
#ifndef QWC_DIAGNOSTICS_PERFT_HPP
#define QWC_DIAGNOSTICS_PERFT_HPP

#include <cstddef>
#include <cstdint>

#include "core/types.hpp"
#include "position/position.hpp"

namespace qwc {

/* A conservative bound on the number of legal moves in one orthodox position:
 * 16 pieces x 27 destinations + 2 en-passant + 2 castles = 436. The true maximum
 * legal-move count in standard chess is well below this, so a buffer of this size
 * lets movegen_legal() never overflow here; an overflow is still treated as a hard
 * failure (never silently truncated). Documented, not tight. */
inline constexpr int kPerftMoveCapacity = 16 * 27 + 2 + 2;   /* 436 */

/* Count the legal move sequences of exactly `depth` from `position`, writing the
 * total into `nodes`. `depth` is in plies (moves).
 *
 * Semantics:
 *  - depth 0 counts the position itself as 1 node (whether or not it is
 *    checkmated or stalemated). A positive-depth node with no legal moves
 *    contributes 0.
 *  - The count is independent of the position's rule-50 / fullmove counters and of
 *    its repetition history: only the diagnostic scratch's counters are normalized
 *    (halfmove 0 / fullmove 1) and its history detached. The board, castling
 *    rights, recorded en-passant square and key are preserved.
 *
 * Returns true and sets `nodes` to the total on success. Returns false and sets
 * `nodes` to 0 on any of: a depth < 0 or > MAX_PLY; a move-generation failure; a
 * make/unmake failure; or a checked u64 count overflow. It never reports a failed
 * traversal as a valid zero. */
[[nodiscard]] bool perft(const Position& position, int depth, u64& nodes);

}  // namespace qwc

#endif /* QWC_DIAGNOSTICS_PERFT_HPP */
