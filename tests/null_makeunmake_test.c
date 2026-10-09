/* QwenChess T004 checkpoint-5 test: the null move (a synthetic pass) and the
 * per-move repetition-history bookkeeping.
 *
 * Groups:
 *  1. null.both_colors_counters -- both colors, no EP; a null changes only the
 *     side/key while preserving every board field and BOTH counters (wide values
 *     incl. 65535); zero edits; a1 retained; unmake restores exactly.
 *  2. null.ep_clearing -- the three audited EP fixtures (+ rank-reflect /
 *     color-swap mirrors); canonical file 3/-1/-1; board byte-identical, final
 *     fields flipped, exact XOR key contract (capable loses the file key),
 *     emitted FEN, and the raw EP restored on undo.
 *  3. null.reject -- ordinary make(0) still fails; a null from check (pawn /
 *     bishop / rook / knight + mirrors) fails and leaves BOTH stored objects
 *     byte-for-byte unchanged.
 *  4. null.nested -- real -> null -> real LIFO undo, double-push -> null ->
 *     EP-restore, and the history pointer moves to the right caller record and
 *     back after every apply/undo (incl. a rejected application).
 *
 * All checks are always active; the harness reports aggregates.
 */
#include "position/position.h"
#include "position/state.h"
#include "position/fen.h"
#include "core/types.h"
#include "core/square.h"
#include "core/zobrist.h"
#include "core/bitboard.h"
#include "test_util.h"
#include <string.h>

/* ---- small helpers -------------------------------------------------------- */

static Square sq_of(int f, int r) { return square_of((Square)f, (Square)r); }
static Move mv(int f, int r, int tf, int tr) { return make_move(sq_of(f, r), sq_of(tf, tr), 0u, 0u); }
static Move mvf(int f, int r, int tf, int tr, int fl, int pr) { return make_move(sq_of(f, r), sq_of(tf, tr), (u32)fl, (u32)pr); }
static Color other(Color c) { return (Color)(1 - (int)c); }

/* Board / derived-caches only (mailbox, per-piece, per-color, occupancy, kings).
 * Deliberately excludes side/cr/ep/counters/key so it can confirm a null left
 * the board byte-identical. */
static int board_equal(const Position *a, const Position *b) {
  for (int s = 0; s < SQ_NB; s++) if (a->mailbox[s] != b->mailbox[s]) return 0;
  for (int p = 0; p < PIECE_NB; p++) if (a->byPiece[p] != b->byPiece[p]) return 0;
  for (int c = 0; c < COLOR_NB; c++) if (a->byColor[c] != b->byColor[c]) return 0;
  if (a->occ != b->occ) return 0;
  if (a->wKingSq != b->wKingSq || a->bKingSq != b->bKingSq) return 0;
  return 1;
}

/* Full logical equality (board + all scalar metadata + derived + key). Does NOT
 * compare the non-owning history pointer (that is checked where it matters). */
static int pos_equal(const Position *a, const Position *b) {
  if (!board_equal(a, b)) return 0;
  if (a->side != b->side) return 0;
  if (a->cr != b->cr) return 0;
  if (a->ep_sq != b->ep_sq) return 0;
  if (a->halfmove != b->halfmove) return 0;
  if (a->fullmove != b->fullmove) return 0;
  if (a->key != b->key) return 0;
  return 1;
}

/* Full undo-record equality, including the history link (for rejection atomicity
 * and round-trip record checks). */
static int state_equal(const StateInfo *a, const StateInfo *b) {
  if (a->prev_key != b->prev_key) return 0;
  if (a->prev_side != b->prev_side) return 0;
  if (a->prev_cr != b->prev_cr) return 0;
  if (a->prev_ep != b->prev_ep) return 0;
  if (a->prev_halfmove != b->prev_halfmove) return 0;
  if (a->prev_fullmove != b->prev_fullmove) return 0;
  if (a->delta.move != b->delta.move) return 0;
  if (a->delta.edit_count != b->delta.edit_count) return 0;
  for (int i = 0; i < MAX_EDIT; i++) {
    if (a->delta.edits[i].sq != b->delta.edits[i].sq) return 0;
    if (a->delta.edits[i].before != b->delta.edits[i].before) return 0;
    if (a->delta.edits[i].after != b->delta.edits[i].after) return 0;
  }
  if (a->prev != b->prev) return 0;
  return 1;
}

/* Do the derived caches + key match a from-scratch rebuild of the same board?
 * pos_rebuild recomputes byPiece/byColor/occ/kings/key (it leaves the recorded
 * ep, side, cr, counters and the history pointer untouched), so this confirms
 * the incremental null produced exactly what a full rebuild would -- key included. */
static int derived_match_rebuild(const Position *pos, const char **why) {
  Position copy = *pos;
  pos_rebuild(&copy);
  for (int p = 0; p < PIECE_NB; p++) if (pos->byPiece[p] != copy.byPiece[p]) { if (why) *why = "byPiece mismatch vs rebuild"; return 0; }
  for (int c = 0; c < COLOR_NB; c++) if (pos->byColor[c] != copy.byColor[c]) { if (why) *why = "byColor mismatch vs rebuild"; return 0; }
  if (pos->occ != copy.occ) { if (why) *why = "occ mismatch vs rebuild"; return 0; }
  if (pos->wKingSq != copy.wKingSq || pos->bKingSq != copy.bKingSq) { if (why) *why = "king square mismatch vs rebuild"; return 0; }
  if (pos->key != copy.key) { if (why) *why = "key mismatch vs rebuild"; return 0; }
  return 1;
}

/* ---- group 1: both colors, no EP, every counter value --------------------- */
static int t_both_colors_counters(void) {
  qwc_begin("null.both_colors_counters");
  const char *why = NULL;
  const int hms[] = { 0, 99, 100, 255, 256, 65535 };
  const int fms[] = { 1, 65535 };   /* covers the normal value and the wide max */
  for (int si = 0; si < 2; si++) {
    Color side = si ? BLACK : WHITE;
    for (int hi = 0; hi < 6; hi++) {
      for (int fi = 0; fi < 2; fi++) {
        Position pos; pos_set_startpos(&pos);
        pos.side = side;
        pos.halfmove = (HalfMoveClock)hms[hi];
        pos.fullmove = (FullMoveNumber)fms[fi];
        pos_rebuild(&pos);   /* keep the key consistent with the overridden side */
        /* startpos: cr == CR_ALL, ep_sq == NO_SQUARE, a1 == W_ROOK */
        StateInfo st; memset(&st, 0, sizeof st);
        Position before = pos;
        char label[64];
        snprintf(label, sizeof label, "null %s hm=%d fm=%d",
                 side == WHITE ? "w" : "b", hms[hi], fms[fi]);
        QWC_TRUE(pos_make_null_move(&pos, &st) == 1, label);
        QWC_TRUE(board_equal(&pos, &before), label);           /* the board is untouched */
        QWC_EQ_I((int)pos.side, (int)other(side), "side flipped");
        QWC_EQ_I((int)pos.mailbox[0], (int)W_ROOK, "a1 still holds the rook");
        QWC_EQ_I(st.delta.edit_count, 0, "zero board edits recorded");
        QWC_EQ_I((int)st.delta.move, 0, "delta move is the null marker");
        QWC_EQ_I((int)pos.halfmove, hms[hi], "halfmove preserved");
        QWC_EQ_I((int)pos.fullmove, fms[fi], "fullmove preserved");
        QWC_EQ_I((int)pos.cr, (int)CR_ALL, "castling rights preserved");
        QWC_EQ_I((int)pos.ep_sq, (int)NO_SQUARE, "ep stays cleared");
        QWC_TRUE(derived_match_rebuild(&pos, &why), "derived/key == rebuild");
        /* the key toggled the side and nothing else (startpos has no ep file) */
        QWC_EQ_U64(pos.key, before.key ^ zobrist_side(before.side) ^ zobrist_side(pos.side),
                   "key = old ^ side(old) ^ side(new)");
        pos_unmake_move(&pos, &st);
        QWC_TRUE(pos_equal(&pos, &before), "unmake restores the Position exactly");
        QWC_TRUE(pos.history == NULL, "unmake restores history to the fresh NULL");
      }
    }
  }
  return qwc_end();
}

/* ---- group 2: EP clearing + exact key contract + FEN + undo --------------- */
static void test_ep_null(const char *fen, int exp_file_before, const char *fen_after) {
  Position pos; StateInfo st; memset(&st, 0, sizeof st);
  const char *why = NULL;
  QWC_TRUE(fen_load(&pos, fen, &why) == 1, "load EP fixture");
  Position before = pos;
  QWC_EQ_I(pos_canon_ep_file(&pos), exp_file_before, "canonical EP file before null");
  QWC_TRUE(pos_make_null_move(&pos, &st) == 1, "null accepted (not in check)");
  QWC_TRUE(board_equal(&pos, &before), "board byte-identical after the null");
  QWC_EQ_I((int)pos.side, (int)other(before.side), "side flipped");
  QWC_EQ_I((int)pos.ep_sq, (int)NO_SQUARE, "recorded EP cleared");
  QWC_EQ_I((int)pos.halfmove, (int)before.halfmove, "halfmove preserved");
  QWC_EQ_I((int)pos.fullmove, (int)before.fullmove, "fullmove preserved");
  QWC_EQ_I((int)pos.cr, (int)before.cr, "castling preserved");
  QWC_EQ_I(st.delta.edit_count, 0, "zero edits");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "caches/key == rebuild");
  /* exact XOR key contract: the side always toggles; the EP-file key leaves only
   * when the recorded EP was capturable (canonical file >= 0). */
  u64 exp_key = before.key ^ zobrist_side(before.side) ^ zobrist_side(pos.side);
  if (exp_file_before >= 0)
    exp_key ^= zobrist_ep_file(exp_file_before);   /* a capturable file key was removed */
  QWC_EQ_U64(pos.key, exp_key, "exact XOR key contract");
  /* the emitted state is the placement + flipped side + '-' + '-' + '0 20' */
  char fbuf[FEN_BUF_SIZE];
  QWC_TRUE(fen_emit(&pos, fbuf, (int)sizeof fbuf) == 1, "emit post-null FEN");
  QWC_TRUE(strcmp(fbuf, fen_after) == 0, "emitted FEN exact");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &before), "unmake restores exactly (raw EP + key)");
  QWC_EQ_I((int)pos.ep_sq, (int)before.ep_sq, "raw EP square restored");
  QWC_EQ_I(pos_canon_ep_file(&pos), exp_file_before, "canonical file restored on undo");
}

static int t_ep_clearing(void) {
  qwc_begin("null.ep_clearing");
  /* the three audited fixtures (canonical files 3 / -1 / -1) ... */
  test_ep_null("k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20",  3, "k7/8/8/3pP3/8/8/8/4K3 b - - 0 20");
  test_ep_null("k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 20", -1, "k3r3/8/8/3pP3/8/8/8/4K3 b - - 0 20");
  test_ep_null("k7/8/8/3p4/8/8/8/4K3 w - d6 0 20",  -1, "k7/8/8/3p4/8/8/8/4K3 b - - 0 20");
  /* ... and their rank-reflected, color-swapped counterparts (same files). */
  test_ep_null("4k3/8/8/8/3Pp3/8/8/K7 b - d3 0 20",     3, "4k3/8/8/8/3Pp3/8/8/K7 w - - 0 20");
  test_ep_null("4k3/8/8/8/3Pp3/8/8/K3R3 b - d3 0 20",   -1, "4k3/8/8/8/3Pp3/8/8/K3R3 w - - 0 20");
  test_ep_null("4k3/8/8/8/3P4/8/8/K7 b - d3 0 20",       -1, "4k3/8/8/8/3P4/8/8/K7 w - - 0 20");
  return qwc_end();
}

/* ---- group 3: rejection (move 0 + every in-check type) is atomic ---------- */
static void check_null_reject(const char *fen, const char *name) {
  Position pos; StateInfo st; memset(&st, 0, sizeof st);
  const char *why = NULL;
  QWC_TRUE(fen_load(&pos, fen, &why) == 1, name);           /* the position itself is valid */
  QWC_TRUE(pos_is_in_check(&pos, pos.side) == 1, name);     /* sanity: it really is in check */
  Position pb = pos; StateInfo sb = st;
  QWC_TRUE(pos_make_null_move(&pos, &st) == 0, name);       /* rejected: a pass cannot leave check */
  QWC_TRUE(board_equal(&pos, &pb), name);                   /* the board is untouched */
  QWC_TRUE(pos_equal(&pos, &pb), name);                     /* every Position field is unchanged */
  QWC_TRUE(state_equal(&st, &sb), name);                    /* the record is byte-for-byte unchanged */
  /* the record was never written (all fields retain their pre-call, zero values) */
  QWC_EQ_I((int)st.delta.move, 0, name);
  QWC_EQ_I(st.delta.edit_count, 0, name);
  QWC_EQ_U64(st.prev_key, 0, name);
  QWC_EQ_I((int)st.prev_side, 0, name);
  QWC_EQ_I((int)st.prev_cr, 0, name);
  QWC_EQ_I((int)st.prev_ep, 0, name);
  QWC_EQ_I((int)st.prev_halfmove, 0, name);
  QWC_EQ_I((int)st.prev_fullmove, 0, name);
  QWC_TRUE(st.prev == NULL, name);
}

static int t_reject(void) {
  qwc_begin("null.reject");
  /* an ordinary make of the 0 move (the no-move sentinel) still fails and is atomic */
  {
    Position pos; pos_set_startpos(&pos);
    StateInfo st; memset(&st, 0, sizeof st);
    Position pb = pos; StateInfo sb = st;
    QWC_TRUE(pos_make_move(&pos, &st, 0) == 0, "ordinary make of move 0 rejected");
    QWC_TRUE(pos_equal(&pos, &pb), "move 0 left the Position unchanged");
    QWC_TRUE(state_equal(&st, &sb), "move 0 left the record unchanged");
  }
  /* a null from check is rejected, for each attack type and its mirror */
  check_null_reject("k7/8/8/3pP3/4K3/8/8/8 w - d6 0 20",   "null rejected (pawn check, w)");
  check_null_reject("8/8/8/4k3/3Pp3/8/8/K7 b - d3 0 20",     "null rejected (pawn check, b)");
  check_null_reject("4b2k/8/8/3pP3/K7/8/8/8 w - d6 0 20",    "null rejected (bishop check, w)");
  check_null_reject("8/8/8/k7/3Pp3/8/8/4B2K b - d3 0 20",    "null rejected (bishop check, b)");
  check_null_reject("rk6/8/8/8/8/8/8/K7 w - - 0 1",            "null rejected (rook check, w)");
  check_null_reject("k7/8/8/8/8/8/8/RK6 b - - 0 1",            "null rejected (rook check, b)");
  check_null_reject("k7/8/8/8/8/1n6/8/K7 w - - 0 1",           "null rejected (knight check, w)");
  check_null_reject("k7/8/1N6/8/8/8/8/K7 b - - 0 1",           "null rejected (knight check, b)");
  return qwc_end();
}

/* ---- group 4: history linking/restoration + nested sequences --------------- */
static int t_nested(void) {
  qwc_begin("null.nested");
  Position pos;
  StateInfo st[8]; memset(st, 0, sizeof st);
  const char *why = NULL;
  QWC_TRUE(fen_load(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &why) == 1, "load startpos");
  Position base = pos;
  QWC_TRUE(pos.history == NULL, "fresh setup: history is NULL");

  /* 1. real -> null -> real, with the history pointer tracking each record +
   *    full LIFO undo back to the root. */
  QWC_TRUE(pos_make_move(&pos, &st[0], mv(6, 0, 5, 2)) == 1, "Nf3 (real) accepted");
  QWC_TRUE(pos.history == &st[0] && st[0].prev == NULL, "history -> st[0] -> root");
  QWC_TRUE(pos_make_null_move(&pos, &st[1]) == 1, "null accepted");
  QWC_TRUE(pos.history == &st[1] && st[1].prev == &st[0], "history -> st[1] -> st[0]");
  QWC_TRUE(pos_make_move(&pos, &st[2], mv(1, 0, 2, 2)) == 1, "Nb1c3 (real) accepted after the null");
  QWC_TRUE(pos.history == &st[2] && st[2].prev == &st[1], "history -> st[2] -> st[1]");
  pos_unmake_move(&pos, &st[2]);
  QWC_TRUE(pos.history == &st[1], "undo st[2] restores history -> st[1]");
  pos_unmake_move(&pos, &st[1]);   /* undo the null */
  QWC_TRUE(pos.history == &st[0], "undo the null restores history -> st[0]");
  pos_unmake_move(&pos, &st[0]);
  QWC_TRUE(pos.history == NULL, "undo st[0] restores history -> NULL");
  QWC_TRUE(pos_equal(&pos, &base), "full unmake restores the startpos exactly");

  /* 2. double push -> null -> undo restores the raw EP square + the history. */
  QWC_TRUE(pos_make_move(&pos, &st[3], mv(4, 1, 4, 3)) == 1, "e4 (double push) accepted");
  QWC_EQ_I((int)pos.ep_sq, (int)sq_of(4, 2), "the e3 EP is recorded");
  QWC_TRUE(pos.history == &st[3] && st[3].prev == NULL, "history -> st[3] -> root");
  QWC_TRUE(pos_make_null_move(&pos, &st[4]) == 1, "null after the push accepted");
  QWC_EQ_I((int)pos.ep_sq, (int)NO_SQUARE, "the null cleared the EP");
  QWC_TRUE(pos.history == &st[4] && st[4].prev == &st[3], "history -> st[4] -> st[3]");
  pos_unmake_move(&pos, &st[4]);
  QWC_EQ_I((int)pos.ep_sq, (int)sq_of(4, 2), "undo null restored the raw EP (e3)");
  QWC_TRUE(pos.history == &st[3], "undo null restores history -> st[3]");
  pos_unmake_move(&pos, &st[3]);
  QWC_TRUE(pos.history == NULL && pos_equal(&pos, &base), "fully restored to the startpos");

  /* 3. special moves also push the chain: an EP capture and a castling. */
  {
    Position p; StateInfo s; memset(&s, 0, sizeof s);
    QWC_TRUE(fen_load(&p, "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20", &why) == 1, "load EP pos");
    Position pb = p;
    QWC_TRUE(pos_make_move(&p, &s, mvf(4, 4, 3, 5, MV_EP, NO_PIECE)) == 1, "EP capture accepted");
    QWC_TRUE(p.history == &s && s.prev == NULL, "EP capture links to the root");
    pos_unmake_move(&p, &s);
    QWC_TRUE(p.history == NULL && pos_equal(&p, &pb), "undo EP restores history + board");
  }
  {
    Position p; StateInfo s; memset(&s, 0, sizeof s);
    QWC_TRUE(fen_load(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 12 42", &why) == 1, "load castling pos");
    Position pb = p;
    QWC_TRUE(pos_make_move(&p, &s, mvf(4, 0, 7, 0, MV_CASTLE, NO_PIECE)) == 1, "O-O accepted");
    QWC_TRUE(p.history == &s && s.prev == NULL, "castling links to the root");
    pos_unmake_move(&p, &s);
    QWC_TRUE(p.history == NULL && pos_equal(&p, &pb), "undo castling restores history + board");
  }

  /* 4. a rejected application leaves the history pointer (and the record link)
   *    exactly where they were. */
  {
    QWC_TRUE(pos.history == NULL, "back at the root before part 4");
    QWC_TRUE(pos_make_move(&pos, &st[5], mv(6, 0, 5, 2)) == 1, "Nf3 accepted");
    QWC_TRUE(pos.history == &st[5], "history -> st[5]");
    memset(&st[6], 0, sizeof st[6]);
    StateInfo s6b = st[6];
    StateInfo *h_before = pos.history;
    QWC_TRUE(pos_make_move(&pos, &st[6], mv(4, 7, 4, 5)) == 0, "bad king move (e8e6) rejected");
    QWC_TRUE(pos.history == h_before, "the rejected move left history unchanged");
    QWC_TRUE(state_equal(&st[6], &s6b), "the rejected move left the record unchanged");
  }
  return qwc_end();
}

int main(void) {
  int fail = 0;
  fail |= t_both_colors_counters();
  fail |= t_ep_clearing();
  fail |= t_reject();
  fail |= t_nested();
  return fail ? 1 : 0;
}
