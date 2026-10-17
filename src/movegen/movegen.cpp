/* QwenChess: non-castling pseudo-legal move generation (T005 checkpoint 1).
 *
 * See movegen.hpp for the contract. Emits every non-castling pseudo-legal
 * candidate: ordinary piece moves/captures, pawn single/double pushes and
 * captures, all four promotions (and capture-promotions), and structural
 * en-passant captures. Own-king safety and castling are out of scope here.
 *
 * Uses the existing attack tables (knight/king static masks; sliders over the
 * combined occupancy) and the Position's by_piece/by_color/occ caches. No heap,
 * no Position copy, no make/unmake, no rebuild, no king-safety query.
 */
#include "movegen/movegen.hpp"

#include <cstddef>

#include "core/types.hpp"
#include "core/square.hpp"
#include "core/bitboard.hpp"
#include "core/attacks.hpp"

namespace qwc {

/* Append one move to `out` if there is room. Returns 0 on overflow (the caller
 * treats that as a hard stop) and never writes outside `capacity`. */
static int mg_append(Move *out, std::size_t capacity, std::size_t *n, Move m) {
  if (*n >= capacity)
    return 0;
  out[*n] = m;
  (*n)++;
  return 1;
}

/* Destination mask for a non-pawn piece on `from`: its attack squares (sliders
 * use the combined occupancy) EXCLUDING friendly occupancy and the opposing
 * king. Own-king safety is NOT applied at this layer. */
static u64 mg_dest_mask(const Position *pos, Square from, PieceType type,
                        u64 friendly, u64 enemy_king_bb) {
  u64 att;
  switch (type) {
    case PieceType::KNIGHT: att = knight_attacks(from); break;
    case PieceType::BISHOP: att = bishop_attacks(from, pos->occ); break;
    case PieceType::ROOK:   att = rook_attacks(from, pos->occ); break;
    case PieceType::QUEEN:  att = queen_attacks(from, pos->occ); break;
    case PieceType::KING:   att = king_attacks(from); break;
    default:     att = 0; break;   /* PieceType::PAWN / PieceType::NO_PIECE_TYPE: handled by the pawn path */
  }
  return att & ~friendly & ~enemy_king_bb;
}

/* Emit the pseudo-legal moves of one friendly pawn on `from`. Covers the single
 * and double push, the two diagonal captures, and (on the final rank) all four
 * promotions for each available push or capture. Returns 0 on capacity overflow
 * (hard stop), 1 otherwise. `dir` is +8 for white / -8 for black; `start_r` and
 * `promo_r` are the (0-indexed) start and promotion ranks for the side. */
static int mg_gen_pawn(const Position *pos, Square from, Move *out, std::size_t capacity,
                       std::size_t *n, int dir, int start_r, int promo_r,
                       u64 enemy, u64 enemy_king_bb) {
  const int f  = file_of(from);
  const int r  = rank_of(from);
  const int tr = r + (dir > 0 ? 1 : -1);   /* the forward target rank index */

  /* Push (same file). Guards against file wrap (none) and rank/edge overflow. */
  if (tr >= 0 && tr < RANK_NB) {
    const Square push = square_of(f, tr);
    if ((pos->occ & square_bb(push)) == 0) {   /* target empty */
      if (tr == promo_r) {
        for (u32 pt = to_under(PieceType::KNIGHT); pt <= to_under(PieceType::QUEEN); pt++)
          if (!mg_append(out, capacity, n, make_move(from, push, (u32)MoveFlag::MV_PROMO, pt)))
            return 0;
      } else {
        if (!mg_append(out, capacity, n, make_move(from, push, 0u, 0u)))
          return 0;
      }
      /* Double push: only from the start rank, both squares empty. (Never to the
       * promotion rank, so a start-rank pawn always has a valid 3rd/6th rank.) */
      if (r == start_r) {
        const Square dbl = square_of(f, tr + (dir > 0 ? 1 : -1));
        if ((pos->occ & square_bb(dbl)) == 0)
          if (!mg_append(out, capacity, n, make_move(from, dbl, 0u, 0u)))
            return 0;
      }
    }
  }

  /* Captures: the two diagonal forward squares; require an enemy non-king piece. */
  for (int sfx = -1; sfx <= 1; sfx += 2) {
    const int cf = f + sfx;
    if (cf < 0 || cf >= FILE_NB)
      continue;                              /* no file wrap */
    if (tr < 0 || tr >= RANK_NB)
      continue;                              /* pawn on the last rank: no forward square */
    const Square to = square_of(cf, tr);
    const u64 to_bb = square_bb(to);
    if ((enemy & to_bb) == 0)
      continue;                              /* needs an enemy piece on the target */
    if (to_bb == enemy_king_bb)
      continue;                              /* never capture the king */
    if (tr == promo_r) {
      for (u32 pt = to_under(PieceType::KNIGHT); pt <= to_under(PieceType::QUEEN); pt++)
        if (!mg_append(out, capacity, n, make_move(from, to, (u32)MoveFlag::MV_PROMO, pt)))
          return 0;
    } else {
      if (!mg_append(out, capacity, n, make_move(from, to, 0u, 0u)))
        return 0;
    }
  }
  return 1;
}

int movegen_pseudo_noncastling(const Position *pos, Move *out, std::size_t capacity,
                               std::size_t *count) {
  if (pos == nullptr || out == nullptr) {
    if (count)
      *count = 0;
    return 0;
  }

  std::size_t n = 0;
  const Color   side    = pos->side;
  const u64     friendly = pos->by_color[to_under(side)];
  const u64     enemy    = pos->by_color[1 - to_under(side)];
  const u64     enemy_king_bb =
      square_bb((side == Color::WHITE) ? pos->black_king_sq : pos->white_king_sq);
  const int  dir     = (side == Color::WHITE) ? 8  : -8;
  const int  start_r = (side == Color::WHITE) ? 1  : 6;   /* 2nd / 7th rank */
  const int  promo_r = (side == Color::WHITE) ? 7  : 0;   /* 8th / 1st rank */
  const u64  side_pawns = pos->by_piece[(side == Color::WHITE) ? to_under(Piece::W_PAWN) : to_under(Piece::B_PAWN)];

  /* Ordinary pieces (knight/bishop/rook/queen/king) + pawns, in a fixed,
   * deterministic square order. */
  for (u64 bits = friendly; bits; bits = pop_lsb(bits)) {
    const Square    from = (Square)lsb_index(bits);
    const PieceType type = piece_type(pos->mailbox[from]);
    if (type == PieceType::PAWN) {
      if (!mg_gen_pawn(pos, from, out, capacity, &n,
                       dir, start_r, promo_r, enemy, enemy_king_bb)) {
        if (count) *count = 0;
        return 0;
      }
    } else {
      const u64 dest = mg_dest_mask(pos, from, type, friendly, enemy_king_bb);
      for (u64 d = dest; d; d = pop_lsb(d)) {
        if (!mg_append(out, capacity, &n,
                       make_move(from, (Square)lsb_index(d), 0u, 0u))) {
          if (count) *count = 0;
          return 0;
        }
      }
    }
  }

  /* En passant: structural, not legal. The recorded target plus its metadata
   * (the just-pushed enemy pawn, an empty landing, an adjacent friendly captor)
   * are checked inline; king safety is deliberately NOT (pos_ep_is_legal /
   * pos_canon_ep_file / pos_ep_capture_is_safe are not consulted). Up to two
   * captors yield two distinct moves; a pinned captor is still a candidate. */
  if (pos->ep_sq != NO_SQUARE) {
    const int ef = file_of(pos->ep_sq);
    const int er = rank_of(pos->ep_sq);
    const int captor_r = er + (side == Color::WHITE ? -1 : 1);   /* one rank behind */
    if (captor_r >= 0 && captor_r < RANK_NB) {
      const u64  cap_bb   = square_bb(square_of(ef, captor_r));  /* the taken pawn */
      const u64  landing  = square_bb(pos->ep_sq);
      const int  enemy_pawn = (side == Color::WHITE) ? to_under(Piece::B_PAWN) : to_under(Piece::W_PAWN);
      const int  enemy_present = (pos->by_piece[enemy_pawn] & cap_bb) != 0;
      const int  landing_empty = (pos->occ & landing) == 0;
      if (enemy_present && landing_empty) {
        for (int sfx = -1; sfx <= 1; sfx += 2) {
          const int cf = ef + sfx;
          if (cf < 0 || cf >= FILE_NB)
            continue;
          const Square captor = square_of(cf, captor_r);
          if ((side_pawns & square_bb(captor)) == 0)
            continue;                              /* needs an adjacent friendly captor */
          if (!mg_append(out, capacity, &n, make_move(captor, pos->ep_sq, (u32)MoveFlag::MV_EP, 0u))) {
            if (count) *count = 0;
            return 0;
          }
        }
      }
    }
  }

  if (count)
    *count = n;
  return 1;
}
}  // namespace qwc
