#include "position/position.hpp"
#include "core/square.hpp"
#include "core/attacks.hpp"
#include "core/zobrist.hpp"
#include <array>

namespace qwc {

void pos_reset(Position *pos) {
  *pos = Position{};
  pos->side = Color::WHITE; pos->ep_sq = NO_SQUARE; pos->fullmove = 1;
  pos_rebuild(pos);   /* consistent empty state: derived fields + key all match */
}

void pos_set_startpos(Position *pos) {
  *pos = Position{};
  /* pawn ranks: white on rank 1 (squares 8..15), black on rank 6 (48..55). */
  for (int f = 0; f < FILE_NB; f++) {
    pos->mailbox[8 + f]  = Piece::W_PAWN;
    pos->mailbox[48 + f] = Piece::B_PAWN;
  }
  /* back ranks (rank 0 = 1st, rank 7 = 8th): R N B Q K B N R */
  pos->mailbox[0] = Piece::W_ROOK; pos->mailbox[1] = Piece::W_KNIGHT; pos->mailbox[2] = Piece::W_BISHOP;
  pos->mailbox[3] = Piece::W_QUEEN; pos->mailbox[4] = Piece::W_KING; pos->mailbox[5] = Piece::W_BISHOP;
  pos->mailbox[6] = Piece::W_KNIGHT; pos->mailbox[7] = Piece::W_ROOK;
  pos->mailbox[56] = Piece::B_ROOK; pos->mailbox[57] = Piece::B_KNIGHT; pos->mailbox[58] = Piece::B_BISHOP;
  pos->mailbox[59] = Piece::B_QUEEN; pos->mailbox[60] = Piece::B_KING; pos->mailbox[61] = Piece::B_BISHOP;
  pos->mailbox[62] = Piece::B_KNIGHT; pos->mailbox[63] = Piece::B_ROOK;
  pos->side = Color::WHITE; pos->cr = CR_ALL; pos->ep_sq = NO_SQUARE;
  pos->halfmove = 0; pos->fullmove = 1;
  pos_rebuild(pos);
}

/* Does piece `p` on `from` attack `to`, given board occupancy `occ`?
 * Slider rays use `occ`; the origin square is excluded by the attack contract.
 * Used only on the cold en-passant canonicalization path. */
static int piece_attacks(Piece p, Square from, Square to, u64 occ) {
  if (from >= SQ_NB || to >= SQ_NB)
    return 0;
  u64 m;
  switch (piece_type(p)) {
    case PieceType::PAWN:   m = pawn_attacks(p >= Piece::B_PAWN ? Color::BLACK : Color::WHITE, from); break;
    case PieceType::KNIGHT: m = knight_attacks(from); break;
    case PieceType::BISHOP: m = bishop_attacks(from, occ); break;
    case PieceType::ROOK:   m = rook_attacks(from, occ); break;
    case PieceType::QUEEN:  m = queen_attacks(from, occ); break;
    case PieceType::KING:   m = king_attacks(from); break;
    default: return 0;
  }
  return (int)((m >> to) & 1);
}

/* King-safety of ONE candidate en-passant capture. Apply it into scratch --
 * remove the captor from its origin, remove the captured pawn from its actual
 * square, add the captor on the target -- rebuild occupancy, and confirm the
 * capturing king (which does not move) is not attacked by any ENEMY piece in the
 * result. The captured pawn is removed first, so it is never left in the enemy
 * pawn-attack set; slider rays use the resulting occupancy. Geometric attacks
 * (including by pinned enemy pieces) are what can make a capture illegal; no
 * legal move generation is performed. FIDE 3.1.3/3.7.3/3.9.2/9.2.3. */
static int ep_capture_is_safe(const Position *pos, Square captor, Square origin,
                              Square target, Piece own_pawn) {
  std::array<Piece, SQ_NB> board{};
  board = pos->mailbox;   /* typed value copy of the authoritative board */
  board[captor]  = Piece::NO_PIECE;   /* captor leaves its origin   */
  board[origin]  = Piece::NO_PIECE;   /* captured pawn removed       */
  board[target]  = own_pawn;   /* captor lands on the target  */
  u64 occ = 0;
  for (int s = 0; s < SQ_NB; s++)
    if (board[s] != Piece::NO_PIECE)
      occ |= square_bb((Square)s);
  Piece want_king = (pos->side == Color::WHITE) ? Piece::W_KING : Piece::B_KING;
  int king = -1;
  for (int s = 0; s < SQ_NB; s++)
    if (board[s] == want_king) { king = s; break; }
  if (king < 0)
    return 0;                                  /* no king: not a legal position */
  for (int s = 0; s < SQ_NB; s++) {
    Piece p = board[s];
    if (p == Piece::NO_PIECE)
      continue;
    if ((p >= Piece::B_PAWN ? Color::BLACK : Color::WHITE) == pos->side)
      continue;                                /* friendly pieces never attack us */
    if (piece_attacks(p, (Square)s, (Square)king, occ))
      return 0;                               /* the capture exposes the king */
  }
  return 1;
}

/* Per-captor king-safety of one en-passant capture (the public form of the
 * static ep_capture_is_safe above): `captor` must be a friendly pawn on the
 * captured pawn's rank, on an adjacent file; anything else is not a captor. The
 * captured pawn and the double-pushed target are derived from the recorded
 * ep square. This is the single source of truth for "is this captor king-safe?"
 * that both pos_ep_is_legal (the OR over captors) and the tests use. */
int pos_ep_capture_is_safe(const Position *pos, Square captor) {
  if (captor >= SQ_NB)
    return 0;
  Square ep = pos->ep_sq;
  if (ep >= SQ_NB || !pos_ep_is_valid_meta(pos))
    return 0;
  int f = file_of(ep), r = rank_of(ep);
  int pr = (pos->side == Color::WHITE) ? r - 1 : r + 1;   /* captured pawn's rank */
  if (pr < 0 || pr >= RANK_NB)
    return 0;
  if (rank_of(captor) != pr)
    return 0;                                   /* captor sits on the pawn's rank */
  int cf = file_of(captor);
  if (cf != f - 1 && cf != f + 1)
    return 0;                                   /* adjacent file only */
  Piece own_pawn = (pos->side == Color::WHITE) ? Piece::W_PAWN : Piece::B_PAWN;
  if (pos->mailbox[captor] != own_pawn)
    return 0;
  return ep_capture_is_safe(pos, captor, square_of(f, pr), ep, own_pawn);
}

/* A fully-legal en-passant capture exists iff the double-pushed enemy pawn is
 * present and at least one of the (up to two) adjacent friendly captors makes a
 * king-safe capture (pos_ep_capture_is_safe). Depends only on the mailbox +
 * side (it locates the king there), so it has no rebuild precondition. Both
 * pawns share one rank `pr`; the ep target square itself is empty. */
int pos_ep_is_legal(const Position *pos) {
  Square ep = pos->ep_sq;
  if (ep >= SQ_NB || !pos_ep_is_valid_meta(pos))
    return 0;
  int f = file_of(ep), r = rank_of(ep);
  int pr = (pos->side == Color::WHITE) ? r - 1 : r + 1;   /* captured pawn's rank */
  if (pr < 0 || pr >= RANK_NB)
    return 0;
  Piece enemy_pawn = (pos->side == Color::WHITE) ? Piece::B_PAWN : Piece::W_PAWN;
  if (pos->mailbox[square_of(f, pr)] != enemy_pawn)
    return 0;                               /* no captured enemy pawn */
  /* Try each adjacent captor (files f-1 and f+1); one legal capture suffices. */
  for (int df = -1; df <= 1; df += 2) {
    if (f + df < 0 || f + df >= FILE_NB)
      continue;
    if (pos_ep_capture_is_safe(pos, square_of(f + df, pr)))
      return 1;
  }
  return 0;
}

int pos_canon_ep_file(const Position *pos) {
  Square ep = pos->ep_sq;
  if (ep >= SQ_NB)
    return -1;
  return pos_ep_is_legal(pos) ? file_of(ep) : -1;
}

/* The recorded ep target must be consistent with a double push just played: the
 * correct rank for the side, an empty destination, the enemy pawn on the
 * just-pushed square, a vacant double-push origin, and a zero halfmove clock.
 * (Legality -- an adjacent, king-safe captor -- is a separate matter; a
 * structurally valid but uncapturable target is a valid Position that simply
 * contributes no ep file to the key.) */
int pos_ep_is_valid_meta(const Position *pos) {
  if (to_under(pos->side) >= COLOR_NB)
    return 0;
  Square ep = pos->ep_sq;
  if (ep == NO_SQUARE)
    return 1;
  if (ep >= SQ_NB)
    return 0;
  int f = file_of(ep), r = rank_of(ep);
  int pr   = (pos->side == Color::WHITE) ? r - 1 : r + 1;   /* captured pawn rank   */
  int orng = (pos->side == Color::WHITE) ? r + 1 : r - 1;   /* double-push origin rank */
  if (r != ((pos->side == Color::WHITE) ? RANK_6 : RANK_3))
    return 0;                                       /* wrong rank for the side */
  if (pr < 0 || pr >= RANK_NB || orng < 0 || orng >= RANK_NB)
    return 0;
  Piece enemy = (pos->side == Color::WHITE) ? Piece::B_PAWN : Piece::W_PAWN;
  if (pos->mailbox[ep] != Piece::NO_PIECE)
    return 0;                                       /* destination not empty */
  if (pos->mailbox[square_of(f, pr)] != enemy)
    return 0;                                       /* no captured pawn */
  if (pos->mailbox[square_of(f, orng)] != Piece::NO_PIECE)
    return 0;                                       /* origin not vacant */
  if (pos->halfmove != 0)
    return 0;                                       /* a double push resets the clock */
  return 1;
}

u64 pos_key(const Position *pos) {
  return zobrist_compute(pos->mailbox.data(), pos->side, pos->cr, pos_canon_ep_file(pos));
}

void pos_rebuild(Position *pos) {
  for (int p = 0; p < PIECE_NB; p++) pos->by_piece[p] = 0;
  pos->by_color[to_under(Color::WHITE)] = 0;
  pos->by_color[to_under(Color::BLACK)] = 0;
  for (int s = 0; s < SQ_NB; s++) {
    int p = to_under(pos->mailbox[s]);
    if (!p || p >= PIECE_NB)
      continue;
    u64 b = square_bb((Square)s);
    pos->by_piece[p] |= b;
    pos->by_color[p >= to_under(Piece::B_PAWN)] |= b;
  }
  pos->occ = pos->by_color[to_under(Color::WHITE)] | pos->by_color[to_under(Color::BLACK)];
  pos->white_king_sq = popcount(pos->by_piece[to_under(Piece::W_KING)]) ? (Square)lsb_index(pos->by_piece[to_under(Piece::W_KING)]) : (Square)NO_SQUARE;
  pos->black_king_sq = popcount(pos->by_piece[to_under(Piece::B_KING)]) ? (Square)lsb_index(pos->by_piece[to_under(Piece::B_KING)]) : (Square)NO_SQUARE;
  pos->key = pos_key(pos);
}

/* The validator inspects into scratch bitboards; it never writes to *pos. */
static int v_kings(const Position *p, const char **why) {
  for (int c = 0; c < COLOR_NB; c++) {
    int k = (c == to_under(Color::WHITE)) ? to_under(Piece::W_KING) : to_under(Piece::B_KING);
    int n = popcount(p->by_piece[k]);
    if (n != 1) { if (why) *why = (c == to_under(Color::WHITE)) ? "white king count != 1" : "black king count != 1"; return 0; }
    Square sq = (Square)lsb_index(p->by_piece[k]);
    if (sq >= SQ_NB) { if (why) *why = "king square out of range"; return 0; }
    Square cs = (c == to_under(Color::WHITE)) ? p->white_king_sq : p->black_king_sq;
    if (cs != sq) { if (why) *why = "cached king square != recomputed"; return 0; }
  }
  return 1;
}
static int v_ep(const Position *p, const char **why) {
  if (p->ep_sq == NO_SQUARE)
    return 1;
  if (p->ep_sq >= SQ_NB) { if (why) *why = "ep square out of range"; return 0; }
  /* Enforce the structural double-push invariant (rank / destination / captured
   * pawn / vacant origin / zero clock). Legality is NOT required here: a valid
   * but uncapturable target is a legal Position that simply contributes no ep
   * file to the key (pos_canon_ep_file -> -1, checked in v_key). */
  if (pos_ep_is_valid_meta(p))
    return 1;
  if (why) *why = "en-passant target is not a valid double-push record";
  return 0;
}
static int v_key(const Position *p, const char **why) {
  u64 recomputed = pos_key(p);
  if (p->key != recomputed) { if (why) *why = "stored key != recomputed key"; return 0; }
  return 1;
}

/* Flip a color (Color::WHITE=0 / Color::BLACK=1). */
static Color other(Color c) { return (Color)(1 - (int)c); }

/* Is `victim`'s king attacked by any enemy piece? Uses geometric attacks (a
 * pinned enemy slider still points at the king). The occupancy is derived from
 * the mailbox so the query is self-consistent for any initialized Position (it
 * does not trust a possibly-stale occupancy cache), and the king square comes
 * from the validated cache. No legal move generation is performed. Cold path.
 * This is the single source of truth for "is this color in check?", used by the
 * validator (v_no_check_nonmover) and by the null move, which rejects a pass
 * while the side to move is in check. */
int pos_is_in_check(const Position *pos, Color victim) {
  Square king = (victim == Color::WHITE) ? pos->white_king_sq : pos->black_king_sq;
  if (king >= SQ_NB)
    return 0;
  u64 occ = 0;
  for (int s = 0; s < SQ_NB; s++)
    if (pos->mailbox[s] != Piece::NO_PIECE)
      occ |= square_bb((Square)s);
  for (int s = 0; s < SQ_NB; s++) {
    Piece p = pos->mailbox[s];
    if (p == Piece::NO_PIECE)
      continue;
    if ((p >= Piece::B_PAWN ? Color::BLACK : Color::WHITE) == victim)
      continue;                                   /* friendly pieces */
    if (piece_attacks(p, (Square)s, king, occ))
      return 1;
  }
  return 0;
}

/* The two kings must not sit on adjacent squares. */
static int v_adjacent_kings(const Position *p, const char **why) {
  if (p->white_king_sq >= SQ_NB || p->black_king_sq >= SQ_NB) { if (why) *why = "missing king for the adjacency check"; return 0; }
  if ((king_attacks(p->white_king_sq) & square_bb(p->black_king_sq)) != 0) { if (why) *why = "kings are on adjacent squares"; return 0; }
  return 1;
}

/* Pawns cannot occupy the first or eighth ranks. */
static int v_pawn_ranks(const Position *p, const char **why) {
  u64 edge = rank_bb(0) | rank_bb(7);
  if ((p->by_piece[to_under(Piece::W_PAWN)] & edge) != 0) { if (why) *why = "a white pawn is on the first/eighth rank"; return 0; }
  if ((p->by_piece[to_under(Piece::B_PAWN)] & edge) != 0) { if (why) *why = "a black pawn is on the first/eighth rank"; return 0; }
  return 1;
}

/* Orthodox material bounds: at most eight pawns and sixteen pieces per color
 * (promoted piece types are allowed, so only the pawn count is the hard cap). */
static int v_piece_counts(const Position *p, const char **why) {
  for (int c = 0; c < COLOR_NB; c++) {
    int pc = (c == to_under(Color::WHITE)) ? to_under(Piece::W_PAWN) : to_under(Piece::B_PAWN);
    if (popcount(p->by_piece[pc]) > 8) { if (why) *why = "a color has more than eight pawns"; return 0; }
    if (popcount(p->by_color[c]) > 16) { if (why) *why = "a color has more than sixteen pieces"; return 0; }
  }
  return 1;
}

/* The side that just moved -- the one NOT to move -- must not be left in check;
 * the side to move may be (the opponent just checked it). */
static int v_no_check_nonmover(const Position *p, const char **why) {
  if (pos_is_in_check(p, other(p->side))) { if (why) *why = "the side not to move is in check"; return 0; }
  return 1;
}

/* Every claimed castling right requires its king and rook on their home squares.
 * Blocked paths or attacked transit squares do NOT erase a right (FIDE 3.9.2 /
 * 9.2.3). The backing check uses only king/rook placement, not ray clearance. */
static int v_cr_backing(const Position *p, const char **why) {
  if ((p->cr & CR_WK) && (p->white_king_sq != square_of(4, 0) || p->mailbox[square_of(7, 0)] != Piece::W_ROOK)) { if (why) *why = "K right unbacked (white king/rook off home)"; return 0; }
  if ((p->cr & CR_WQ) && (p->white_king_sq != square_of(4, 0) || p->mailbox[square_of(0, 0)] != Piece::W_ROOK)) { if (why) *why = "Q right unbacked (white king/rook off home)"; return 0; }
  if ((p->cr & CR_BK) && (p->black_king_sq != square_of(4, 7) || p->mailbox[square_of(7, 7)] != Piece::B_ROOK)) { if (why) *why = "k right unbacked (black king/rook off home)"; return 0; }
  if ((p->cr & CR_BQ) && (p->black_king_sq != square_of(4, 7) || p->mailbox[square_of(0, 7)] != Piece::B_ROOK)) { if (why) *why = "q right unbacked (black king/rook off home)"; return 0; }
  return 1;
}

int pos_validate(const Position *pos, const char **why) {
  if (why) *why = nullptr;
  u64 eOcc = 0, eW = 0, eB = 0;
  for (int s = 0; s < SQ_NB; s++) {
    int p = to_under(pos->mailbox[s]);
    if (p != to_under(Piece::NO_PIECE) && !((p >= to_under(Piece::W_PAWN) && p <= to_under(Piece::W_KING)) ||
                          (p >= to_under(Piece::B_PAWN) && p <= to_under(Piece::B_KING)))) {
      if (why) *why = "mailbox holds an invalid piece code";
      return 0;
    }
    if (p) {
      u64 b = square_bb((Square)s);
      eOcc |= b;
      if (p >= to_under(Piece::B_PAWN)) eB |= b; else eW |= b;
    }
  }
  for (int c = 0; c < COLOR_NB; c++)
    if (pos->by_color[c] != (c == to_under(Color::WHITE) ? eW : eB)) { if (why) *why = "color occupancy != recompute"; return 0; }
  if (pos->occ != eOcc) { if (why) *why = "occupancy != recompute"; return 0; }
  if (pos->by_piece[to_under(Piece::NO_PIECE)] != 0) { if (why) *why = "empty-piece bitboard must be zero"; return 0; }
  for (int p = 1; p < PIECE_NB; p++) {
    u64 e = 0;
    for (int s = 0; s < SQ_NB; s++)
      if (to_under(pos->mailbox[s]) == p) e |= square_bb((Square)s);
    if (pos->by_piece[p] != e) { if (why) *why = "piece bitboard != recompute"; return 0; }
  }
  if (!v_kings(pos, why)) return 0;
  if ((int)pos->side >= COLOR_NB) { if (why) *why = "invalid side to move"; return 0; }
  if ((int)pos->cr > 15) { if (why) *why = "invalid castling rights"; return 0; }
  if (!v_adjacent_kings(pos, why)) return 0;
  if (!v_pawn_ranks(pos, why)) return 0;
  if (!v_piece_counts(pos, why)) return 0;
  if (!v_cr_backing(pos, why)) return 0;
  /* HalfMoveClock's entire u16 range is representable; draw policy is separate. */
  if (pos->fullmove < 1) { if (why) *why = "fullmove number must be >= 1"; return 0; }
  if (!v_ep(pos, why)) return 0;
  if (!v_no_check_nonmover(pos, why)) return 0;
  if (!v_key(pos, why)) return 0;
  return 1;
}
}  // namespace qwc
