/* QwenChess: the production UCI coordinate-move codec (implementation).
 *
 * See uci_codec.hpp for the contract. Parse resolution runs the complete legal
 * generator once per token (a command-thread cold path, not the search hot path) and
 * matches the token against the resulting set, so en-passant / promotion / castle
 * flags are derived from the legal move itself. Emit writes the documented lowercase
 * UCI form; a castle is written with the king's g/c destination, never the internal
 * home-rook origin.
 */
#include "engine/uci_codec.hpp"

#include <array>

namespace qwc {

[[nodiscard]] u32 uci_promo_from_char(char c) {
  switch (c) {
    case 'q': case 'Q': return to_under(PieceType::QUEEN);    /* 5 */
    case 'r': case 'R': return to_under(PieceType::ROOK);     /* 4 */
    case 'b': case 'B': return to_under(PieceType::BISHOP);   /* 3 */
    case 'n': case 'N': return to_under(PieceType::KNIGHT);   /* 2 */
    default:            return 0;                             /* invalid */
  }
}

[[nodiscard]] char uci_char_from_promo(u32 t) {
  switch (t) {
    case to_under(PieceType::QUEEN):  return 'q';
    case to_under(PieceType::ROOK):   return 'r';
    case to_under(PieceType::BISHOP): return 'b';
    case to_under(PieceType::KNIGHT): return 'n';
    default:                          return 0;
  }
}

/* The two characters of a square ("e2") -> the square, or NO_SQUARE if either
 * character is not a valid file ('a'-'h', case-insensitive) or rank ('1'-'8'). */
static Square parse_two(const char* p) {
  int f = (p[0] >= 'a' && p[0] <= 'h') ? (p[0] - 'a')
          : (p[0] >= 'A' && p[0] <= 'H') ? (p[0] - 'A') : -1;
  int r = (p[1] >= '1' && p[1] <= '8') ? (p[1] - '1') : -1;
  if (f < 0 || r < 0)
    return NO_SQUARE;
  return square_of(f, r);
}

/* For a castle token the wire 'to' is the king's g/c destination; map it to the
 * internal home-rook origin (h/a) so it can be matched against the legal set. For
 * any other (from,to) the internal 'to' is the wire 'to' itself. */
static Square internal_to_of(Square from, Square to) {
  bool home_king = (from == square_of(4, 0)) || (from == square_of(4, 7));   /* e1 / e8 */
  if (is_valid_square(from) && is_valid_square(to) && home_king &&
      rank_of(from) == rank_of(to)) {
    int tf = file_of(to);
    if (tf == 6)
      return square_of(7, rank_of(from));   /* g -> h (kingside) */
    if (tf == 2)
      return square_of(0, rank_of(from));   /* c -> a (queenside) */
  }
  return to;
}

[[nodiscard]] bool uci_move_parse(const Position& pos, std::string_view token, Move& out) {
  if (token.size() != 4 && token.size() != 5)
    return false;   /* malformed length; `out` unchanged */
  Square from = parse_two(token.data());
  Square to   = parse_two(token.data() + 2);
  if (!is_valid_square(from) || !is_valid_square(to) || from == to)
    return false;   /* bad squares, or 0000-style no-op; `out` unchanged */

  u32 promo = 0;
  if (token.size() == 5) {
    promo = uci_promo_from_char(token[4]);
    if (promo == 0)
      return false;   /* a fifth character must name a promotion piece */
  }

  Square ito = internal_to_of(from, to);   /* castle g/c -> h/a; else the wire 'to' */

  std::array<Move, kUciMoveCapacity> moves{};
  std::size_t n = 0;
  if (!movegen_legal(pos, moves, n))
    return false;   /* the Position must be legal; never invent a move */

  for (std::size_t i = 0; i < n; ++i) {
    Move m = moves[i];
    if (move_from(m) != from || move_to(m) != ito)
      continue;
    if (promo == 0 ? is_promotion(m)
                   : (!is_promotion(m) || move_promo(m) != promo))
      continue;
    out = m;
    return true;
  }
  return false;   /* pinned / illegal / wrong promotion: `out` unchanged */
}

[[nodiscard]] int uci_move_emit(Move m, char* buf, std::size_t cap) {
  if (cap < kUciMoveText)
    return 0;   /* nothing written; the buffer cannot hold a move + NUL */
  Square from = move_from(m);
  if (!is_valid_square(from))
    return 0;   /* the no-move sentinel (0000) is emitted by the protocol layer, not here */
  Square to;
  if (is_castle(m)) {
    /* internal 'to' is the home-rook origin (h/a); write the king's g/c destination. */
    int tf = file_of(move_to(m));
    to = square_of(tf == 7 ? 6 : 2, rank_of(from));   /* h->g / a->c on the king's rank */
  } else {
    to = move_to(m);
  }
  buf[0] = file_char(file_of(from));
  buf[1] = rank_char(rank_of(from));
  buf[2] = file_char(file_of(to));
  buf[3] = rank_char(rank_of(to));
  if (is_promotion(m)) {
    buf[4] = uci_char_from_promo(move_promo(m));
    buf[5] = '\0';
    return 5;
  }
  buf[4] = '\0';
  return 4;
}

}  // namespace qwc
