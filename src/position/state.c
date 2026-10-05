/* QwenChess: reversible move application (make/unmake) -- T004 checkpoints 1+2+3+4.
 *
 * Implements the one-Position / caller-StateInfo reversible-move contract for
 * ORDINARY moves (checkpoint 1: quiet moves, ordinary captures, pawn
 * single/double pushes, ordinary pawn captures), EN-PASSANT captures (checkpoint
 * 2), orthodox CASTLING (checkpoint 3: the 4-edit application in the apply path
 * below), and PROMOTIONS/underpromotions (checkpoint 4: the 2-edit application).
 * Null moves are the final T004 checkpoint and are still rejected cleanly here
 * (reserved in the delta's capacity).
 *
 * Everything here is chess-level: no NNUE types, feature indices or evaluator
 * state. make() validates every input, records the before-state + a bounded
 * per-square delta in the caller's StateInfo, then applies the move
 * incrementally (no rebuild, no heap, no whole-Position snapshot). unmake()
 * reverses the delta and restores the recorded scalars, so every logical field
 * and the key come back exact.
 *
 * An en-passant capture reverses through THREE square edits: the captor leaves
 * its origin, the captured enemy pawn (one rank toward the captor, on the
 * target's file) is removed, and the captor lands on the (empty) recorded target.
 * Orthodox castling reverses through FOUR square edits: the king leaves its
 * origin, the rook leaves its origin, the king lands on its destination, and the
 * rook lands on its destination. A promotion reverses through TWO square edits:
 * the pawn leaves its origin, and the destination (empty for a straight promo,
 * the victim for a capture-promo) receives the promoted piece of the mover's
 * color. The apply path loops over the populated edits and the unmake path
 * reverts them in order, so the generic delta handles 2, 3 and 4 edits alike.
 *
 * EP, castling and promotion application are STRUCTURAL, not legal: each requires
 * the correct pieces, geometry and (for castling) the right, but none rejects
 * king safety. A pinned captor's EP, a check-exposing castle (FIDE 3.8.2) or a
 * pinned promotion is still applied and reversed here; T005 filters them. For
 * castling, T005 must check the king's origin, transit AND destination squares
 * (a final-position make/check/unmake alone misses castling out of / through
 * check).
 */
#include "position/state.h"
#include "position/position.h"
#include "core/square.h"
#include "core/attacks.h"
#include "core/zobrist.h"
#include <stddef.h>

/* ---- small piece helpers ------------------------------------------------- */

static Color piece_color(Piece p)   { return (p >= B_PAWN) ? BLACK : WHITE; }
static Color other(Color c)         { return (Color)(1 - (int)c); }
static int   piece_is_pawn(Piece p) { return (p & 7) == PAWN; }

/* ---- shared incremental derived-cache maintenance ------------------------
 * make() and unmake() both update the derived caches square-by-square through
 * these, so neither ever rebuilds the whole board. A single add/remove leaves a
 * self-consistent Position (mailbox, byPiece, byColor, occupancy, king squares).
 */

/* Place piece `p` on `sq` (callers clear the square first when replacing). */
static void pos_add_piece(Position *pos, Square sq, Piece p) {
  if (p == NO_PIECE || sq >= SQ_NB)
    return;
  u64 b = square_bb(sq);
  pos->mailbox[sq] = p;
  pos->byPiece[p] |= b;
  pos->byColor[p >= B_PAWN] |= b;
  pos->occ |= b;
  if (p == W_KING) pos->wKingSq = sq;
  else if (p == B_KING) pos->bKingSq = sq;
}

/* Remove whatever piece is on `sq`, if any. */
static void pos_remove_piece(Position *pos, Square sq) {
  if (sq >= SQ_NB)
    return;
  Piece p = pos->mailbox[sq];
  if (p == NO_PIECE)
    return;
  u64 b = square_bb(sq);
  pos->mailbox[sq] = NO_PIECE;
  pos->byPiece[p] &= ~b;
  pos->byColor[p >= B_PAWN] &= ~b;
  pos->occ &= ~b;
  if (p == W_KING && pos->wKingSq == sq) pos->wKingSq = NO_SQUARE;
  else if (p == B_KING && pos->bKingSq == sq) pos->bKingSq = NO_SQUARE;
}

/* Recompute both king squares from the piece bitboards. Run after every
 * make/unmake so the cached squares are exact no matter the transient edit order
 * (a consistent position always has exactly one king per color). */
static void pos_update_kings(Position *pos) {
  pos->wKingSq = pos->byPiece[W_KING] ? (Square)lsb_index(pos->byPiece[W_KING]) : (Square)NO_SQUARE;
  pos->bKingSq = pos->byPiece[B_KING] ? (Square)lsb_index(pos->byPiece[B_KING]) : (Square)NO_SQUARE;
}

/* Apply / revert one square edit (before -> after, or after -> before). */
static void apply_edit(Position *pos, const SquareEdit *e) {
  pos_remove_piece(pos, e->sq);
  pos_add_piece(pos, e->sq, e->after);
}
static void revert_edit(Position *pos, const SquareEdit *e) {
  pos_remove_piece(pos, e->sq);
  pos_add_piece(pos, e->sq, e->before);
}

/* ---- castling-rights transitions -----------------------------------------
 * A king move drops both of its rights, a home-rook move drops that rook's
 * right, and capturing an enemy home rook drops that (enemy) right. Unrelated
 * rights are preserved. cr_clear keeps the arithmetic in u8 to stay conversion-
 * clean. Castling (checkpoint 3) reuses this with a NO_PIECE `dest`: the mover
 * is the king, so the king-move rule clears BOTH of its rights (the intended
 * result); the friendly rook at the encoded destination is deliberately not
 * passed as `dest`, so it is never mistaken for a captured enemy rook. */
static u8 cr_clear(u8 cr, int mask) { return (u8)cr & (u8)~mask; }

static u8 cr_after_move(u8 cr, Square from, Square to, Piece mover, Piece dest) {
  if (mover == W_KING)      cr = cr_clear(cr, CR_WK | CR_WQ);
  else if (mover == B_KING) cr = cr_clear(cr, CR_BK | CR_BQ);
  /* the rook that left a home square forfeits its right */
  if (from == square_of(0, 0))      cr = cr_clear(cr, CR_WQ);   /* a1 */
  else if (from == square_of(7, 0)) cr = cr_clear(cr, CR_WK);   /* h1 */
  else if (from == square_of(0, 7)) cr = cr_clear(cr, CR_BQ);   /* a8 */
  else if (from == square_of(7, 7)) cr = cr_clear(cr, CR_BK);   /* h8 */
  /* capturing an enemy rook on a home square erases that enemy right */
  if (dest != NO_PIECE) {
    if (to == square_of(0, 0))      cr = cr_clear(cr, CR_WQ);
    else if (to == square_of(7, 0)) cr = cr_clear(cr, CR_WK);
    else if (to == square_of(0, 7)) cr = cr_clear(cr, CR_BQ);
    else if (to == square_of(7, 7)) cr = cr_clear(cr, CR_BK);
  }
  return cr;
}

/* ---- en-passant record ----------------------------------------------------
 * A double pawn push records the square it passed (so a later EP capture is
 * detectable and the key can canonicalize it); every other move clears it. The
 * record is retained even when no capture is actually legal; only the key
 * canonicalizes it (pos_canon_ep_file). */
static int is_double_push(Color side, Square from, Square to) {
  if ((from & 7) != (to & 7))
    return 0;                                  /* different file: not a push */
  int rf = rank_of(from);
  int start = (side == WHITE) ? RANK_2 : RANK_7;
  if (rf != start)
    return 0;                                  /* not on its starting rank */
  int rt = rank_of(to);
  return (rt == rf + ((side == WHITE) ? 2 : -2));
}

static Square passed_square(Color side, Square from) {
  int f = file_of(from);
  int r = rank_of(from) + ((side == WHITE) ? 1 : -1);
  return square_of((Square)f, (Square)r);
}

/* ---- move validation (every input is checked before any mutation) --------- */

/* Does the move have a plausible path for `mover` to reach `to`? Sliders use the
 * combined occupancy (the first blocker counts, so captures qualify); a king is
 * adjacent; a pawn advances forward. Pseudo-legality only -- own-king safety is
 * movegen's concern in T005. */
static int geometry_ok(const Position *pos, Square from, Square to, Piece mover, Piece dest) {
  u64 occ = pos->occ;
  int rf = rank_of(from), rt = rank_of(to);
  int ff = file_of(from),  tf = file_of(to);
  int is_cap = (dest != NO_PIECE);

  switch ((int)(mover & 7)) {
    case PAWN: {
      int dir = (pos->side == WHITE) ? 1 : -1;   /* one rank forward */
      int fwd = rf + dir;
      if (tf == ff && rt == fwd && dest == NO_PIECE)
        return 1;                                /* single push */
      int start = (pos->side == WHITE) ? RANK_2 : RANK_7;
      if (tf == ff && rt == rf + 2 * dir && rf == start && dest == NO_PIECE &&
          pos->mailbox[square_of((Square)ff, (Square)fwd)] == NO_PIECE)
        return 1;                                /* double push */
      if (rt == fwd && (tf == ff - 1 || tf == ff + 1) && is_cap)
        return 1;                                /* ordinary capture */
      return 0;
    }
    case KNIGHT: return (knight_attacks(from) & square_bb(to)) != 0;
    case BISHOP: return (bishop_attacks(from, occ) & square_bb(to)) != 0;
    case ROOK:   return (rook_attacks(from, occ) & square_bb(to)) != 0;
    case QUEEN:  return (queen_attacks(from, occ) & square_bb(to)) != 0;
    case KING:   return (king_attacks(from) & square_bb(to)) != 0;
    default:     return 0;                       /* NO_PIECE cannot move (source validated) */
  }
}

/* A pawn whose destination is the final rank MUST promote. An MV_PROMO-flagged
 * move is handled by promo_supported (checkpoint 4); this catches a pawn move
 * that reaches the last rank WITHOUT the MV_PROMO flag, which is invalid. */
static int reaches_last_rank(Color side, Square to) {
  int rt = rank_of(to);
  return (side == WHITE) ? (rt == RANK_8) : (rt == RANK_1);
}

/* Orthodox castling (checkpoint 3): structural application preconditions.
 * Validates, before any write, in order: the flag is EXACTLY MV_CASTLE (a mixed
 * EP|CASTLE / PROMO|CASTLE is rejected on that bit) with a zero promotion
 * payload; the mover is the friendly king on its orthodox home square (e-file,
 * the 1st rank for white / 8th for black); the encoded destination is that
 * side's rook origin (the h-file for kingside, the a-file for queenside, on the
 * home rank) and holds the friendly rook -- a move encoded king-to-king
 * (e1g1/e1c1/e8g8/e8c8) is NON-orthodox under this encoding and is rejected; the
 * corresponding right is present; every required empty square on the home rank is
 * actually empty (kingside f,g; queenside b,c,d -- the b-file square included);
 * and the resulting counters fit (a castle is a quiet move: the halfmove clock
 * increments, and a black castle increments the fullmove number).
 *
 * It deliberately does NOT consult king safety: attacks on the king's origin,
 * transit or destination (FIDE 3.8.2) and a rook origin / b-file square that is
 * attacked are NOT rejected here -- that is movegen's concern in T005. A
 * structurally valid castle that leaves the king in check is still applied and
 * reversed; T005 must check all three king squares when it generates castling. */
static int castle_supported(const Position *pos, Move m, Square from, Square to, const char **why) {
  if (why) *why = NULL;
  if (move_flags(m) != MV_CASTLE) { if (why) *why = "castling move carries extra flags (mixed)"; return 0; }
  if (move_promo(m) != 0)         { if (why) *why = "promotion payload on a castling move"; return 0; }
  Color side = pos->side;
  Piece want_king = (side == WHITE) ? W_KING : B_KING;
  if (pos->mailbox[from] != want_king) { if (why) *why = "castling source is not the friendly king"; return 0; }
  int home_rank = (side == WHITE) ? RANK_1 : RANK_8;   /* 1st rank white, 8th black */
  if (rank_of(from) != home_rank)    { if (why) *why = "castling origin is not on the home rank"; return 0; }
  if (file_of(from) != FILE_E)       { if (why) *why = "castling origin is not the e-file"; return 0; }
  if (rank_of(to) != home_rank)      { if (why) *why = "castling destination is not on the home rank"; return 0; }
  int kingside = (file_of(to) == FILE_H);
  if (!kingside && file_of(to) != FILE_A) { if (why) *why = "castling destination is not a rook origin (non-orthodox)"; return 0; }
  Piece want_rook = (side == WHITE) ? W_ROOK : B_ROOK;
  if (pos->mailbox[to] != want_rook) { if (why) *why = "castling destination does not hold the friendly rook"; return 0; }
  u8 want_cr = (side == WHITE) ? (kingside ? CR_WK : CR_WQ) : (kingside ? CR_BK : CR_BQ);
  if ((pos->cr & want_cr) == 0)     { if (why) *why = "the corresponding castling right is missing"; return 0; }
  if (kingside) {
    if (pos->mailbox[square_of(FILE_F, (Square)home_rank)] != NO_PIECE) { if (why) *why = "kingside f-square is not empty"; return 0; }
    if (pos->mailbox[square_of(FILE_G, (Square)home_rank)] != NO_PIECE) { if (why) *why = "kingside g-square is not empty"; return 0; }
  } else {
    if (pos->mailbox[square_of(FILE_B, (Square)home_rank)] != NO_PIECE) { if (why) *why = "queenside b-square is not empty"; return 0; }
    if (pos->mailbox[square_of(FILE_C, (Square)home_rank)] != NO_PIECE) { if (why) *why = "queenside c-square is not empty"; return 0; }
    if (pos->mailbox[square_of(FILE_D, (Square)home_rank)] != NO_PIECE) { if (why) *why = "queenside d-square is not empty"; return 0; }
  }
  /* A castle is a quiet move (no pawn, no capture): the halfmove clock increments,
   * so a maxed clock would overflow; a black castle increments the fullmove number.
   * A white castle leaves the fullmove unchanged, so a maxed white fullmove is fine. */
  if (pos->halfmove == UINT16_MAX)              { if (why) *why = "halfmove clock would overflow"; return 0; }
  if (side == BLACK && pos->fullmove == UINT16_MAX) { if (why) *why = "fullmove number would overflow"; return 0; }
  return 1;
}

/* En-passant capture (checkpoint 2): pseudo-legal application preconditions.
 * Validates, before any write: the flag is EXACTLY MV_EP (no mixed EP|PROMO /
 * EP|CASTLE) with a zero promotion payload; the move targets the RECORDED ep
 * square; the recorded record is a structurally valid double push (correct rank
 * for the side, an empty destination, the enemy pawn on the just-pushed square,
 * a vacant origin, and a zero halfmove clock -- the existing metadata contract,
 * checked via pos_ep_is_valid_meta); the captor is a friendly pawn moving
 * diagonally one file and one rank forward; an enemy pawn sits on the captured
 * square (one rank toward the captor on the target's file); and the resulting
 * fullmove number fits its u16 storage. It deliberately does NOT consult
 * pos_ep_is_legal / pos_ep_capture_is_safe -- those add a king-safety query that
 * is a move-generation concern (T005), not an application invariant; a pinned
 * captor's structurally valid capture is still applied and reversed. */
static int ep_supported(const Position *pos, Move m, Square from, Square to, const char **why) {
  if (why) *why = NULL;
  if (move_flags(m) != MV_EP)    { if (why) *why = "EP move carries extra flags (mixed)"; return 0; }
  if (move_promo(m) != 0)        { if (why) *why = "promotion payload on an EP move"; return 0; }
  Square ep = pos->ep_sq;
  if (to != ep)                  { if (why) *why = "EP move does not target the recorded ep square"; return 0; }
  if (ep >= SQ_NB)               { if (why) *why = "no recorded en-passant target"; return 0; }
  if (!pos_ep_is_valid_meta(pos)) { if (why) *why = "malformed en-passant record"; return 0; }
  Color  side = pos->side;
  Piece  own_pawn = (side == WHITE) ? W_PAWN : B_PAWN;
  if (pos->mailbox[from] != own_pawn) { if (why) *why = "EP captor is not the friendly pawn"; return 0; }
  int rf = rank_of(from), rt = rank_of(to);
  int dir = (side == WHITE) ? 1 : -1;                    /* one rank forward */
  if (rt != rf + dir)                     { if (why) *why = "EP capture rank is not one forward"; return 0; }
  if (rt != ((side == WHITE) ? RANK_6 : RANK_3)) { if (why) *why = "EP capture target is not on the capture rank"; return 0; }
  int ff = file_of(from);
  if (file_of(to) != ff - 1 && file_of(to) != ff + 1) { if (why) *why = "EP capture file is not one diagonal"; return 0; }
  /* the captured pawn sits one rank toward the captor on the target's file */
  int cs = (side == WHITE) ? (int)to - 8 : (int)to + 8;
  if (cs < 0 || cs >= SQ_NB)             { if (why) *why = "captured square out of range"; return 0; }
  Piece enemy_pawn = (side == WHITE) ? B_PAWN : W_PAWN;
  if (pos->mailbox[cs] != enemy_pawn)    { if (why) *why = "no enemy pawn on the capture square"; return 0; }
  /* EP is a pawn move (and a capture): the clock resets to 0, so no halfmove
   * overflow; a black move increments the fullmove number, which must fit. */
  if (side == BLACK && pos->fullmove == UINT16_MAX) { if (why) *why = "fullmove number would overflow"; return 0; }
  return 1;
}

/* Promotion / underpromotion (checkpoint 4): pseudo-legal application
 * preconditions. Validates, before any write, in order: the flag is EXACTLY
 * MV_PROMO (a mixed EP|PROMO / CASTLE|PROMO is rejected on that bit; such a
 * combination is actually routed to the EP or castling validator first, which
 * rejects it there); the promo payload is a real promotion target (KNIGHT..QUEEN
 * -- 0, PAWN, KING and 7 are rejected; the encoder's value is used, never
 * defaulted to queen); the mover is the friendly pawn on its penultimate rank
 * (white 7th / black 2nd); the destination is one rank further forward (the
 * final rank); and the move is a same-file push onto an empty square, or a
 * one-file diagonal onto an enemy NON-KING piece -- a diagonal to an empty
 * square, a straight push onto an occupied square, a friendly victim, a king
 * victim, a file wrap, and a backward / two-rank / wrong-rank displacement are
 * all rejected. A promo resets the halfmove clock to 0, so no halfmove overflow
 * is possible; only a black promo increments the fullmove number, which must fit
 * its u16 storage.
 *
 * It deliberately does NOT consult the mover's king safety: a structurally valid
 * pinned promotion is still applied and reversed; T005 filters it. Choosing a
 * piece type does not depend on what is captured, or how many friendly pieces of
 * that type already exist. [FIDE Laws 3.7.3.3-3.7.3.5] */
static int promo_supported(const Position *pos, Move m, Square from, Square to, const char **why) {
  if (why) *why = NULL;
  if (move_flags(m) != MV_PROMO)    { if (why) *why = "promotion move carries extra flags (mixed)"; return 0; }
  u32 pt = move_promo(m);
  if (pt < (u32)KNIGHT || pt > (u32)QUEEN) { if (why) *why = "promotion target is not a legal piece type"; return 0; }
  Color side = pos->side;
  Piece own_pawn = (side == WHITE) ? W_PAWN : B_PAWN;
  if (pos->mailbox[from] != own_pawn) { if (why) *why = "promotion source is not the friendly pawn"; return 0; }
  int rf = rank_of(from), rt = rank_of(to);
  int dir = (side == WHITE) ? 1 : -1;                     /* one rank forward */
  int penult = (side == WHITE) ? RANK_7 : RANK_2;        /* the rank before the final */
  if (rf != penult)                 { if (why) *why = "promotion origin is not on the penultimate rank"; return 0; }
  if (rt != penult + dir)           { if (why) *why = "promotion target is not the final rank"; return 0; }
  int ff = file_of(from);
  Piece dest = pos->mailbox[to];
  if (file_of(to) == ff) {
    if (dest != NO_PIECE)          { if (why) *why = "straight promotion onto an occupied square"; return 0; }
  } else if (file_of(to) == ff - 1 || file_of(to) == ff + 1) {
    if (dest == NO_PIECE)              { if (why) *why = "diagonal promotion onto an empty square"; return 0; }
    if (piece_color(dest) == side)     { if (why) *why = "diagonal promotion captures a friendly piece"; return 0; }
    if (dest == W_KING || dest == B_KING) { if (why) *why = "a king cannot be captured"; return 0; }
  } else {
    if (why) *why = "promotion displacement is not a push or a single diagonal";
    return 0;
  }
  if (side == BLACK && pos->fullmove == UINT16_MAX) { if (why) *why = "fullmove number would overflow"; return 0; }
  return 1;
}

/* Is `m` a move the make() path supports, plausibly playable on *pos? Pure read.
 * Every precondition is checked here; only if it returns 1 does make() write
 * anything -- so a 0 return leaves *pos and *state byte-for-byte untouched. */
static int move_supported(const Position *pos, Move m, const char **why) {
  if (why) *why = NULL;
  if (pos == NULL)                          { if (why) *why = "null Position"; return 0; }
  if (is_null_move(m))                      { if (why) *why = "null move"; return 0; }
  if ((m >> 15) & 1u)                       { if (why) *why = "reserved bit 15 set"; return 0; }
  if (!move_repr_is_valid(m))               { if (why) *why = "reserved bits 19..31 set"; return 0; }
  Square from = move_from(m), to = move_to(m);
  if (from == to)                           { if (why) *why = "from == to"; return 0; }
  /* Castling is supported (checkpoint 3): route an MV_CASTLE-flagged move to the
   * structural validator. It is checked before the ordinary destination/capture
   * checks because the encoded destination holds the friendly ROOK (which those
   * checks would reject as an own piece); the validator itself requires the flag
   * to be exactly MV_CASTLE, a zero payload, the friendly king/rook, the right,
   * and an empty path. */
  if (is_castle(m))
    return castle_supported(pos, m, from, to, why);
  /* En-passant is supported (checkpoint 2): route an EP-flagged move to the EP
   * validator, which requires the flag to be exactly MV_EP and a zero payload. */
  if (is_ep(m))
    return ep_supported(pos, m, from, to, why);
  /* Promotion is supported (checkpoint 4): route an MV_PROMO-flagged move to the
   * promo validator, which requires the flag to be exactly MV_PROMO, a legal
   * promo PieceType (KNIGHT..QUEEN), a friendly pawn one rank forward to the
   * final rank, and the push/diagonal geometry above. */
  if (is_promotion(m))
    return promo_supported(pos, m, from, to, why);
  if (move_promo(m) != 0)                   { if (why) *why = "promotion data on an ordinary move"; return 0; }

  Piece mover = pos->mailbox[from];
  if (mover == NO_PIECE)                    { if (why) *why = "source square is empty"; return 0; }
  if (piece_color(mover) != pos->side)      { if (why) *why = "source piece is not the side to move"; return 0; }
  Piece dest = pos->mailbox[to];
  if (dest != NO_PIECE && piece_color(dest) == pos->side)
                                             { if (why) *why = "destination holds an own piece"; return 0; }
  if (dest == W_KING || dest == B_KING)     { if (why) *why = "a king cannot be captured"; return 0; }

  if (!geometry_ok(pos, from, to, mover, dest)) { if (why) *why = "invalid piece path"; return 0; }
  /* A plain (non-MV_PROMO) pawn move that reaches the last rank must promote;
   * without the flag it is invalid. (MV_PROMO moves were routed to promo_supported
   * above, so they never reach this check.) */
  if (piece_is_pawn(mover) && reaches_last_rank(pos->side, to))
                                    { if (why) *why = "pawn reaches the last rank without the promotion flag"; return 0; }

  int is_reset = piece_is_pawn(mover) || (dest != NO_PIECE);
  if (!is_reset && pos->halfmove == UINT16_MAX) { if (why) *why = "halfmove clock would overflow"; return 0; }
  if (pos->side == BLACK && pos->fullmove == UINT16_MAX) { if (why) *why = "fullmove number would overflow"; return 0; }
  return 1;
}

/* ---- public: make ---------------------------------------------------------
 * Applies `m` to *pos (checkpoint 1-4 move kinds: ordinary, en-passant,
 * castling, promotion) and records the undo state in *state. Returns 1 on
 * success; on any failure returns 0 with *pos and *state byte-for-byte unchanged
 * (move_supported runs before the first write). */
int pos_make_move(Position *pos, StateInfo *state, Move m) {
  const char *why = NULL;
  if (pos == NULL || state == NULL)
    return 0;
  if (!move_supported(pos, m, &why))
    return 0;   /* validated; nothing was written, so both outputs stay untouched */

  Square from = move_from(m), to = move_to(m);
  Piece mover = pos->mailbox[from];
  Piece dest  = pos->mailbox[to];        /* NO_PIECE for quiet/EP; the rook for a castle */
  Color side = pos->side;                /* the mover's color (before the flip) */
  int is_ep_mv = is_ep(m);
  int is_castle_mv = is_castle(m);
  int is_promo_mv = is_promotion(m);
  /* A castle is a quiet move (not a capture): its encoded destination holds the
   * friendly rook, so exclude that from is_cap. For EP the destination is empty
   * (is_cap 0) and the clock resets via the pawn flag instead. */
  int is_cap = (dest != NO_PIECE) && !is_castle_mv;
  int is_pawn_mv = piece_is_pawn(mover);

  /* record the before-state + the bounded board delta in the undo record */
  state->prev_key      = pos->key;
  state->prev_side     = (u8)side;
  state->prev_cr       = pos->cr;
  state->prev_ep       = pos->ep_sq;
  state->prev_halfmove = pos->halfmove;
  state->prev_fullmove = pos->fullmove;
  state->delta.move       = m;

  if (is_castle_mv) {
    /* Orthodox castling: FOUR distinct squares change. The king leaves its origin,
     * the rook leaves its origin, the king lands on its destination, and the rook
     * lands on its destination. `mover` is the king at `from`; `dest` is the rook
     * at `to` (the encoded rook origin). Both destinations share the home rank and
     * derive from the flank: kingside king->g / rook->f, queenside king->c / rook->d.
     * Every square is validated by castle_supported (friendly king+rook, the right,
     * and an empty path), so the two destination squares are empty here. */
    int kr = rank_of(from);
    int kf = (file_of(to) == FILE_H) ? FILE_G : FILE_C;   /* king destination file */
    int rf = (file_of(to) == FILE_H) ? FILE_F : FILE_D;   /* rook destination file */
    Piece king = mover;
    Piece rook = dest;
    state->delta.edit_count = 4;
    state->delta.edits[0].sq = from;                    state->delta.edits[0].before = king;   state->delta.edits[0].after = NO_PIECE;  /* king origin */
    state->delta.edits[1].sq = to;                      state->delta.edits[1].before = rook;   state->delta.edits[1].after = NO_PIECE;  /* rook origin */
    state->delta.edits[2].sq = square_of(kf, kr);       state->delta.edits[2].before = NO_PIECE; state->delta.edits[2].after = king;   /* king dest */
    state->delta.edits[3].sq = square_of(rf, kr);       state->delta.edits[3].before = NO_PIECE; state->delta.edits[3].after = rook;   /* rook dest */
  } else if (is_ep_mv) {
    /* En-passant capture: three distinct squares change. The captor leaves its
     * origin; the captured enemy pawn (one rank toward the captor, on the
     * target's file) is removed; the captor lands on the (empty) recorded
     * target. The captured square is validated by ep_supported; here it is
     * derived the same way (signed, then narrowed -- `to` is a validated 6-bit
     * board square, so to-8 / to+8 stay in the board for a capture on ranks 3/6). */
    Square victim = (Square)((side == WHITE) ? (int)to - 8 : (int)to + 8);
    Piece enemy_pawn = (side == WHITE) ? B_PAWN : W_PAWN;
    state->delta.edit_count = 3;
    state->delta.edits[0].sq = from;   state->delta.edits[0].before = mover;      state->delta.edits[0].after = NO_PIECE;
    state->delta.edits[1].sq = victim; state->delta.edits[1].before = enemy_pawn; state->delta.edits[1].after = NO_PIECE;
    state->delta.edits[2].sq = to;     state->delta.edits[2].before = NO_PIECE;   state->delta.edits[2].after = mover;
  } else if (is_promo_mv) {
    /* Promotion / underpromotion: TWO distinct squares change. The pawn leaves its
     * origin; the destination (empty for a straight promo, the victim for a
     * capture-promo) receives the promoted piece of the mover's color. The promo
     * payload is a legal PieceType (KNIGHT..QUEEN), so the board piece is
     * payload + 8*side. The destination already held the victim (or was empty),
     * so no third edit is needed: a capture-promo replaces the victim in place. */
    Piece promoted = (Piece)(move_promo(m) + (side == BLACK ? 8 : 0));
    state->delta.edit_count = 2;
    state->delta.edits[0].sq = from; state->delta.edits[0].before = mover; state->delta.edits[0].after = NO_PIECE;
    state->delta.edits[1].sq = to;   state->delta.edits[1].before = dest;  state->delta.edits[1].after = promoted;
  } else {
    /* Ordinary move (quiet or capture): origin loses the piece, destination
     * gains it (replacing whatever was there, NO_PIECE for a quiet move). */
    state->delta.edit_count = 2;
    state->delta.edits[0].sq = from; state->delta.edits[0].before = mover; state->delta.edits[0].after = NO_PIECE;
    state->delta.edits[1].sq = to;   state->delta.edits[1].before = dest;  state->delta.edits[1].after = mover;
  }

  int old_ep_file = pos_canon_ep_file(pos);   /* canonical ep of the BEFORE position */

  /* apply every board edit, then the metadata transitions (the loop handles 2,
   * 3 and 4-edit moves alike) */
  for (int i = 0; i < state->delta.edit_count; i++)
    apply_edit(pos, &state->delta.edits[i]);
  pos_update_kings(pos);
  /* Castling clears BOTH of the moving side's rights (the mover is the king). Pass
   * NO_PIECE as the `dest` for a castle so the friendly rook at the encoded
   * destination is never mistaken for a captured enemy rook. */
  pos->cr = cr_after_move(pos->cr, from, to, mover, is_castle_mv ? NO_PIECE : dest);
  /* A double push records the passed square; every other move (incl. castling,
   * never a push) clears the recorded en-passant target. */
  pos->ep_sq = is_double_push(side, from, to) ? passed_square(side, from) : (Square)NO_SQUARE;
  /* A castle is a quiet move: is_pawn_mv and is_cap are both 0, so the clock
   * increments (overflow validated in castle_supported); the fullmove number
   * increments for black only. */
  int nhm = (is_pawn_mv || is_cap) ? 0 : (int)pos->halfmove + 1;  /* validated <= 65535 */
  pos->halfmove = (HalfMoveClock)nhm;
  int nfm = (side == BLACK) ? (int)pos->fullmove + 1 : (int)pos->fullmove;
  pos->fullmove = (FullMoveNumber)nfm;
  pos->side = other(side);

  /* incremental key: XOR the before->after deltas into the recorded key */
  u64 key = state->prev_key;
  for (int i = 0; i < state->delta.edit_count; i++) {
    const SquareEdit *e = &state->delta.edits[i];
    if (e->before != NO_PIECE) key ^= zobrist_piece(e->before, e->sq);
    if (e->after  != NO_PIECE) key ^= zobrist_piece(e->after,  e->sq);
  }
  key ^= zobrist_side((Color)state->prev_side) ^ zobrist_side(pos->side);
  key ^= zobrist_castling(state->prev_cr) ^ zobrist_castling(pos->cr);
  int new_ep_file = pos_canon_ep_file(pos);   /* canonical ep of the AFTER position */
  key ^= (old_ep_file >= 0 ? zobrist_ep_file(old_ep_file) : 0);
  key ^= (new_ep_file >= 0 ? zobrist_ep_file(new_ep_file) : 0);
  pos->key = key;
  return 1;
}

/* ---- public: unmake -------------------------------------------------------
 * Reverses the move recorded in *state, restoring *pos's whole logical state
 * (board caches, king squares, side, castling, recorded ep, both counters and
 * the key) exactly to what it was before the matching make. *state is untouched
 * and may be reused. Must be called in strict LIFO order. */
void pos_unmake_move(Position *pos, StateInfo *state) {
  if (pos == NULL || state == NULL)
    return;
  const MoveDelta *d = &state->delta;
  for (int i = 0; i < d->edit_count; i++)
    revert_edit(pos, &d->edits[i]);
  pos_update_kings(pos);
  pos->cr        = state->prev_cr;
  pos->ep_sq     = state->prev_ep;
  pos->halfmove  = state->prev_halfmove;
  pos->fullmove  = state->prev_fullmove;
  pos->side      = (Color)state->prev_side;
  pos->key       = state->prev_key;
}
