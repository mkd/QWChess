/* QwenChess: the temporary Material static evaluation (implementation).
 *
 * See material.hpp for the contract. This is a pure read of the Position's derived
 * piece bitboards (by_piece) + its side: it accumulates the signed material of all
 * sixteen piece bitboards (white positive, black negative -- the sign is encoded in
 * kPieceValue) and then negates the sum when it is black to move. It allocates
 * nothing and mutates nothing (the Position, its history and its records are read
 * only). No tempo bonus and no piece-square table.
 */
#include "search/material.hpp"

#include <cstddef>

#include "core/bitboard.hpp"

namespace qwc {

Value eval_material(const Position& pos) {
  /* Accumulate the signed material over all piece bitboards in the WHITE perspective
   * (the table encodes the sign). A given piece type holds at most sixteen pieces,
   * and each value is at most 900 in magnitude, so a single product is at most
   * 14400 and the full sum at most kMaterialMax (4000) -- well inside an int32.
   * by_piece[0] is NO_PIECE (always empty) so it contributes 0. */
  Value white = VALUE_ZERO;
  for (std::size_t p = 0; p < PIECE_NB; ++p)
    white += (Value)popcount(pos.by_piece[p]) * kPieceValue[p];

  /* Side-to-move perspective: white to move keeps the sign; black to move negates. */
  return pos.side == Color::WHITE ? white : -white;
}

}  // namespace qwc
