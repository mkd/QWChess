/* QwenChess: reversible move application (make/unmake) -- T004 checkpoint 1.
 *
 * Implements the one-Position / caller-StateInfo reversible-move contract for
 * ORDINARY moves: quiet moves, ordinary captures, pawn single/double pushes and
 * ordinary pawn captures. Castling, promotions and en-passant capture are later
 * checkpoints and are rejected cleanly here (reserved in the delta's capacity).
 *
 * Everything here is chess-level: no NNUE types, feature indices or evaluator
 * state. make() validates every input, records the before-state + a bounded
 * per-square delta in the caller's StateInfo, then applies the move
 * incrementally (no rebuild, no heap, no whole-Position snapshot). unmake()
 * reverses the delta and restores the recorded scalars, so every logical field
 * and the key come back exact.
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
 * Required now even though castling itself is a later checkpoint: a king move
 * drops both of its rights, a home-rook move drops that rook's right, and
 * capturing an enemy home rook drops that (enemy) right. Unrelated rights are
 * preserved. cr_clear keeps the arithmetic in u8 to stay conversion-clean. */
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

/* A pawn whose destination is the final rank would have to promote (checkpoint 3);
 * unsupported here, so reject every pawn move reaching the last rank. */
static int reaches_last_rank(Color side, Square to) {
  int rt = rank_of(to);
  return (side == WHITE) ? (rt == RANK_8) : (rt == RANK_1);
}

/* Is `m` a move checkpoint 1 supports, plausibly playable on *pos? Pure read.
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
  if (is_ep(m))                             { if (why) *why = "en-passant capture is a later checkpoint"; return 0; }
  if (is_castle(m))                        { if (why) *why = "castling is a later checkpoint"; return 0; }
  if (is_promotion(m))                      { if (why) *why = "promotion is a later checkpoint"; return 0; }
  if (move_promo(m) != 0)                   { if (why) *why = "promotion data on an ordinary move"; return 0; }

  Piece mover = pos->mailbox[from];
  if (mover == NO_PIECE)                    { if (why) *why = "source square is empty"; return 0; }
  if (piece_color(mover) != pos->side)      { if (why) *why = "source piece is not the side to move"; return 0; }
  Piece dest = pos->mailbox[to];
  if (dest != NO_PIECE && piece_color(dest) == pos->side)
                                             { if (why) *why = "destination holds an own piece"; return 0; }
  if (dest == W_KING || dest == B_KING)     { if (why) *why = "a king cannot be captured"; return 0; }

  if (!geometry_ok(pos, from, to, mover, dest)) { if (why) *why = "invalid piece path"; return 0; }
  if (piece_is_pawn(mover) && reaches_last_rank(pos->side, to))
                                             { if (why) *why = "mandatory promotion is a later checkpoint"; return 0; }

  int is_reset = piece_is_pawn(mover) || (dest != NO_PIECE);
  if (!is_reset && pos->halfmove == UINT16_MAX) { if (why) *why = "halfmove clock would overflow"; return 0; }
  if (pos->side == BLACK && pos->fullmove == UINT16_MAX) { if (why) *why = "fullmove number would overflow"; return 0; }
  return 1;
}

/* ---- public: make ---------------------------------------------------------
 * Applies `m` to *pos (checkpoint-1 move kinds) and records the undo state in
 * *state. Returns 1 on success; on any failure returns 0 with *pos and *state
 * byte-for-byte unchanged (move_supported runs before the first write). */
int pos_make_move(Position *pos, StateInfo *state, Move m) {
  const char *why = NULL;
  if (pos == NULL || state == NULL)
    return 0;
  if (!move_supported(pos, m, &why))
    return 0;   /* validated; nothing was written, so both outputs stay untouched */

  Square from = move_from(m), to = move_to(m);
  Piece mover = pos->mailbox[from];
  Piece dest  = pos->mailbox[to];        /* NO_PIECE for a quiet move */
  Color side = pos->side;                /* the mover's color (before the flip) */
  int is_cap = (dest != NO_PIECE);
  int is_pawn_mv = piece_is_pawn(mover);

  /* record the before-state + the bounded board delta in the undo record */
  state->prev_key      = pos->key;
  state->prev_side     = (u8)side;
  state->prev_cr       = pos->cr;
  state->prev_ep       = pos->ep_sq;
  state->prev_halfmove = pos->halfmove;
  state->prev_fullmove = pos->fullmove;
  state->delta.move       = m;
  state->delta.edit_count = 2;
  state->delta.edits[0].sq = from; state->delta.edits[0].before = mover; state->delta.edits[0].after = NO_PIECE;
  state->delta.edits[1].sq = to;   state->delta.edits[1].before = dest;  state->delta.edits[1].after = mover;

  int old_ep_file = pos_canon_ep_file(pos);   /* canonical ep of the BEFORE position */

  /* apply the board edits, then the metadata transitions */
  apply_edit(pos, &state->delta.edits[0]);
  apply_edit(pos, &state->delta.edits[1]);
  pos_update_kings(pos);
  pos->cr = cr_after_move(pos->cr, from, to, mover, dest);
  pos->ep_sq = is_double_push(side, from, to) ? passed_square(side, from) : (Square)NO_SQUARE;
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
