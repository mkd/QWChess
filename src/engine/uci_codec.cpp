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
#include <cctype>

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

/* The wire 'to' square of a legal candidate: a castle's wire destination is the
 * KING's g/c square (the internal 'to' is the home-ROOK origin h/a); every other
 * move uses its ordinary destination. Matching the token against the candidate's
 * wire form (never remapping the token's squares) is what keeps an ORDINARY move
 * from a home-king square -- e.g. a rook's e1-g1 when the king is elsewhere -- from
 * being confused with the internal castle encoding e1-h1. */
static Square wire_to_of(Move m) {
  if (is_castle(m))
    return square_of(file_of(move_to(m)) == 7 ? 6 : 2, rank_of(move_from(m)));   /* h->g / a->c */
  return move_to(m);
}

/* A UCI castling keyword ("O-O" / "O-O-O", either letter case) -> its side. The
 * UCI specification's canonical form is the king's destination square (e1g1); the
 * O-O spellings are the accepted alternative and resolve to the same move. The
 * comparison is LOWERCASED on both sides, so any letter case matches. */
static bool castle_keyword(std::string_view token, bool queenside) {
  static const char kKing[]  = "o-o";
  static const char kQueen[] = "o-o-o";
  const char* kw = queenside ? kQueen : kKing;
  const std::size_t n = queenside ? 5 : 3;
  if (token.size() != n)
    return false;
  for (std::size_t i = 0; i < n; ++i)
    if (std::tolower((unsigned char)token[i]) != kw[i])
      return false;
  return true;
}

[[nodiscard]] bool uci_move_parse(const Position& pos, std::string_view token, Move& out) {
  /* The castling keywords are resolved against the legal set FIRST: a known spelling
   * for a castle that is not currently legal fails exactly like any other illegal
   * token (`out` unchanged). */
  if (token.size() == 3 || token.size() == 5) {
    const bool queenside = castle_keyword(token, true);
    if (queenside || castle_keyword(token, false)) {
      std::array<Move, kUciMoveCapacity> moves{};
      std::size_t n = 0;
      if (movegen_legal(pos, moves, n)) {
        const Square kfrom = pos.side == Color::WHITE ? square_of(4, 0)
                                                      : square_of(4, 7);
        for (std::size_t i = 0; i < n; ++i) {
          const Move m = moves[i];
          if (!is_castle(m) || move_from(m) != kfrom)
            continue;
          const bool king_side = file_of(move_to(m)) == 7;   /* internal 'to' = the home rook */
          if (king_side == !queenside) {
            out = m;
            return true;
          }
        }
      }
      return false;
    }
  }

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

  std::array<Move, kUciMoveCapacity> moves{};
  std::size_t n = 0;
  if (!movegen_legal(pos, moves, n))
    return false;   /* the Position must be legal; never invent a move */

  for (std::size_t i = 0; i < n; ++i) {
    Move m = moves[i];
    if (move_from(m) != from || wire_to_of(m) != to)
      continue;   /* a candidate's canonical wire form must equal the token's squares */
    if (promo == 0 ? is_promotion(m)
                   : (!is_promotion(m) || move_promo(m) != promo))
      continue;
    out = m;
    return true;
  }
  return false;   /* pinned / illegal / internal castle spelling / wrong promotion: `out` unchanged */
}

[[nodiscard]] int uci_move_emit(Move m, char* buf, std::size_t cap) {
  if (cap < kUciMoveText)
    return 0;   /* nothing written; the buffer cannot hold a move + NUL */
  if (m == move_none()) {
    /* the explicit output sentinel for a root with no legal move (checkmate or
     * stalemate), or an internal search failure with no known legal move: 0000.
     * It must be written here -- move_from(move_none()) is square 0 (a1), so
     * falling through would corrupt the emission into a bogus "a1a1". */
    buf[0] = '0'; buf[1] = '0'; buf[2] = '0'; buf[3] = '0'; buf[4] = '\0';
    return 4;
  }
  Square from = move_from(m);
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
