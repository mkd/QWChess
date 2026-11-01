/* QwenChess: the temporary Material static evaluation.
 *
 * LABELLED TEMPORARY (see AGENTS.md + docs/PLAN.md T006): a deliberately weak,
 * allocation-free, read-only static evaluation used ONLY to develop the search in
 * this milestone. It is NOT a NNUE evaluator, is never a fallback when a network
 * cannot load, and must never be presented as the engine's real evaluation. The real
 * NNUE evaluator lands in M3 and replaces this at the search horizon.
 *
 * The score is the piece-material balance in centipawns, from the side-to-move
 * perspective, with the fixed values of this checkpoint (no tempo bonus, no
 * piece-square table):
 *
 *   Pawn 100   Knight 320   Bishop 330   Rook 500   Queen 900   King 0
 *
 * White material counts positive, black negative; the result is negated when it is
 * black to move. The maximum possible one-sided material is 8*100 + 2*320 + 2*330 +
 * 2*500 + 900 = 4000 cp, so the score is ALWAYS strictly inside the reserved
 * tablebase/mate interval (VALUE_TB_LOSS_IN_MAX_PLY, VALUE_TB_WIN_IN_MAX_PLY) and can
 * never collide with a mate or tablebase score (static_assert below). The evaluator
 * does NOT adjudicate mate, stalemate or repetition -- that is the search's job; it
 * only reports the static material balance of a nonterminal position.
 *
 * Ownership/lifetime: a pure read of the Position's derived piece bitboards
 * (by_piece) + its side. It allocates nothing and never mutates *pos, its history
 * or any record. The bitboards are the Position's authoritative derived caches, kept
 * consistent by pos_rebuild / make / unmake, so no recomputation is needed here.
 */
#ifndef QWC_SEARCH_MATERIAL_HPP
#define QWC_SEARCH_MATERIAL_HPP

#include <cstddef>

#include "core/types.hpp"
#include "position/position.hpp"

namespace qwc {

/* The fixed piece values (centipawns) of this checkpoint, indexed by the frozen
 * one-byte Piece code (0..15). White pieces (1..6) are positive, black (9..14)
 * negative; the empty square (0) and the in-table-but-unnamed slots (7, 8, 15) and
 * the kings are 0. The values mirror the task specification exactly. */
inline constexpr Value kPieceValue[PIECE_NB] = {
    /* 0  */       0,   /* NO_PIECE */
    /* 1  */     100,   /* W_PAWN   */
    /* 2  */     320,   /* W_KNIGHT */
    /* 3  */     330,   /* W_BISHOP */
    /* 4  */     500,   /* W_ROOK   */
    /* 5  */     900,   /* W_QUEEN  */
    /* 6  */       0,   /* W_KING   */
    /* 7  */       0,   /* (in-table, unnamed) */
    /* 8  */       0,   /* (in-table, unnamed) */
    /* 9  */    -100,   /* B_PAWN   */
    /* 10 */    -320,   /* B_KNIGHT */
    /* 11 */    -330,   /* B_BISHOP */
    /* 12 */    -500,   /* B_ROOK   */
    /* 13 */    -900,   /* B_QUEEN  */
    /* 14 */       0,   /* B_KING   */
    /* 15 */       0,   /* (in-table, unnamed) */
};

/* The maximum absolute material a single side can hold (a full, un-promoted army):
 * 8 pawns + 2 knights + 2 bishops + 2 rooks + 1 queen. The eval can never exceed
 * this in magnitude, so it is strictly inside the reserved TB/mate interval and can
 * never be mistaken for a mate/TB score. Pinned at compile time. */
inline constexpr Value kMaterialMax =
    8 * kPieceValue[1] + 2 * kPieceValue[2] + 2 * kPieceValue[3]
    + 2 * kPieceValue[4] + kPieceValue[5];   /* == 4000 cp */

static_assert(kMaterialMax < VALUE_TB_WIN_IN_MAX_PLY &&
              -kMaterialMax > VALUE_TB_LOSS_IN_MAX_PLY,
              "material eval must stay strictly inside the reserved TB/mate ranges");

/* The temporary static material evaluation of `pos`, in centipawns, from the
 * side-to-move perspective (white to move -> the white-black material balance;
 * black to move -> its negation). A pure read of the derived piece bitboards + side;
 * it allocates nothing and leaves *pos, its history and its records byte-for-byte
 * unchanged. For the orthodox start position (equal material) this is 0. It reports
 * the static balance only -- mate, stalemate and repetition are decided by search. */
[[nodiscard]] Value eval_material(const Position& pos);

}  // namespace qwc

#endif /* QWC_SEARCH_MATERIAL_HPP */
