/* QwenChess T004 checkpoint-3 test: castling make/unmake.
 *
 * Exercises the reversible orthodox castling that checkpoint 3 adds to
 * pos_make_move/pos_unmake_move: a 4-edit structural application (king
 * origin -> empty, rook origin -> empty, empty king destination -> king,
 * empty rook destination -> rook) with both of the moving side's rights
 * cleared, the recorded en-passant target cleared, the halfmove clock
 * incremented, the moving king's cache updated, the hash rebuilt exactly,
 * and a full restore on unmake.
 *
 * Castling here is STRUCTURAL, not legal: the make path validates the pieces,
 * the claimed right, the frozen king-origin-to-rook-origin encoding and the
 * empty path, but NOT the king's safety (FIDE 3.8.2) -- that is movegen's job
 * in T005. A successful make is therefore not a legality certificate.
 *
 * All checks use the always-active QWC_* macros (they hold under NDEBUG too)
 * and compare defined fields, never struct padding. */
#include "position/position.h"
#include "position/state.h"
#include "position/fen.h"
#include "core/types.h"
#include "core/square.h"
#include "test_util.h"
#include <string.h>

#define FEN_BEFORE_W "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 7 20"
#define FEN_BEFORE_B "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 7 20"

/* ---- small helpers (mirrored from ep_makeunmake_test.c) ------------------- */
static Square sq_of(int f, int r) { return square_of((Square)f, (Square)r); }
static Move   mvf(int f, int r, int tf, int tr, int fl, int pr) {
  return make_move(sq_of(f, r), sq_of(tf, tr), (u32)fl, (u32)pr);
}
static int pos_equal(const Position *a, const Position *b) {
  for (int s = 0; s < SQ_NB; s++) if (a->mailbox[s] != b->mailbox[s]) return 0;
  if (a->side != b->side) return 0;
  if (a->cr != b->cr) return 0;
  if (a->ep_sq != b->ep_sq) return 0;
  if (a->halfmove != b->halfmove) return 0;
  if (a->fullmove != b->fullmove) return 0;
  for (int p = 0; p < PIECE_NB; p++) if (a->byPiece[p] != b->byPiece[p]) return 0;
  for (int c = 0; c < COLOR_NB; c++) if (a->byColor[c] != b->byColor[c]) return 0;
  if (a->occ != b->occ) return 0;
  if (a->wKingSq != b->wKingSq) return 0;
  if (a->bKingSq != b->bKingSq) return 0;
  if (a->key != b->key) return 0;
  return 1;
}
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
  return 1;
}
/* Do the derived caches + key match a from-scratch rebuild of the same board?
 * pos_rebuild recomputes byPiece/byColor/occ/kings/key (it leaves the recorded
 * ep, side, cr and counters untouched), so this confirms the incremental make()
 * produced exactly what a full rebuild would -- including the key and the
 * moving king's cached square. */
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

typedef struct { Square sq; Piece pc; } PC;

/* Build a fresh Position from an explicit piece list + the given metadata, then
 * rebuild the derived caches. (mkpos does NOT validate; it is for fabricating
 * boards that fen_load would refuse -- e.g. an unbacked right, a missing rook,
 * or an enemy king on a castling destination.) */
static Position mkpos(Color side, int cr, int hm, int fm, int ep, const PC *pcs, int n) {
  Position p; pos_reset(&p);
  p.side = side; p.cr = (u8)cr; p.halfmove = (HalfMoveClock)hm; p.fullmove = (FullMoveNumber)fm;
  p.ep_sq = (Square)ep;
  for (int i = 0; i < n; i++) p.mailbox[pcs[i].sq] = pcs[i].pc;
  pos_rebuild(&p);
  return p;
}

/* The standard castling fixture: both home rooks + both kings on their home
 * squares (a1 e1 h1 / a8 e8 h8), everything else empty. */
static const PC std6[6] = {
  { 0,  W_ROOK }, { 4,  W_KING }, { 7,  W_ROOK },
  { 56, B_ROOK }, { 60, B_KING }, { 63, B_ROOK },
};
/* Build the standard 6-piece board with a given side/cr/counters. */
static Position castle_board(Color side, int cr, int hm, int fm) {
  Position p; pos_reset(&p);
  p.side = side; p.cr = (u8)cr; p.halfmove = (HalfMoveClock)hm; p.fullmove = (FullMoveNumber)fm; p.ep_sq = NO_SQUARE;
  for (int i = 0; i < 6; i++) p.mailbox[std6[i].sq] = std6[i].pc;
  pos_rebuild(&p);
  return p;
}

/* A make must be rejected and must leave BOTH the Position and an initialized
 * StateInfo byte-for-byte unchanged (the contract for every rejection). */
static void assert_reject(Position *pos, Move m) {
  Position before = *pos;
  StateInfo st; memset(&st, 0, sizeof st);
  StateInfo st0 = st;
  QWC_TRUE(pos_make_move(pos, &st, m) == 0, "make rejected");
  QWC_TRUE(pos_equal(pos, &before), "pos unchanged after reject");
  QWC_TRUE(state_equal(&st, &st0), "state unchanged after reject");
}
static int reject_fen_case(const char *tag, const char *fen, Move m) {
  qwc_begin(tag);
  Position pos; const char *why = NULL;
  QWC_TRUE(fen_load(&pos, fen, &why) == 1, "fixture loads");
  assert_reject(&pos, m);
  return qwc_end();
}
static int reject_mkpos_case(const char *tag, Color side, int cr, int hm, int fm, int ep,
                             const PC *pcs, int n, Move m) {
  qwc_begin(tag);
  Position pos = mkpos(side, cr, hm, fm, ep, pcs, n);
  assert_reject(&pos, m);
  return qwc_end();
}
/* ---- structural facts ------------------------------------------------------ */
static int t_sizes(void) {
  qwc_begin("castle.sizes");
  printf("      sizeof(Square)=%zu sizeof(Piece)=%zu sizeof(Move)=%zu sizeof(Color)=%zu "
         "sizeof(HalfMoveClock)=%zu sizeof(FullMoveNumber)=%zu\n",
         sizeof(Square), sizeof(Piece), sizeof(Move), sizeof(Color),
         sizeof(HalfMoveClock), sizeof(FullMoveNumber));
  printf("      sizeof(Position)=%zu sizeof(StateInfo)=%zu sizeof(MoveDelta)=%zu\n",
         sizeof(Position), sizeof(StateInfo), sizeof(MoveDelta));
  QWC_TRUE(sizeof(StateInfo) < sizeof(Position), "StateInfo < Position");
  QWC_TRUE(sizeof(MoveDelta) < sizeof(StateInfo), "MoveDelta < StateInfo");
  QWC_TRUE(sizeof(MoveDelta) <= 64, "MoveDelta <= 64 bytes");
  QWC_EQ_I((int)MAX_EDIT, 4, "MAX_EDIT capacity = 4 (a castle uses all four)");
  return qwc_end();
}

static int t_exact_one(const char *tag, const char *before, Move m,
                       int kf, int kr, int rf, int rr,
                       int kto_f, int rto_f, Piece king, Piece rook, const char *after) {
  qwc_begin(tag);
  Position pos; StateInfo st; const char *why = NULL; char buf[FEN_BUF_SIZE];
  memset(&st, 0, sizeof st);
  int hrank = (king == W_KING) ? RANK_1 : RANK_8;
  Square kfrom = sq_of(kf, kr), rfrom = sq_of(rf, rr);
  Square kto = sq_of(kto_f, hrank), rto = sq_of(rto_f, hrank);
  int other_home = (king == W_KING) ? 60 : 4;   /* e8 (black) or e1 (white) */
  QWC_TRUE(fen_load(&pos, before, &why) == 1, "load before");
  Position base = pos;
  QWC_TRUE(pos_make_move(&pos, &st, m) == 1, "castle accepted");
  QWC_EQ_I((int)pos.mailbox[kfrom], (int)NO_PIECE, "king origin now empty");
  QWC_EQ_I((int)pos.mailbox[rfrom], (int)NO_PIECE, "rook origin now empty");
  QWC_EQ_I((int)pos.mailbox[kto], (int)king, "king relocated to destination");
  QWC_EQ_I((int)pos.mailbox[rto], (int)rook, "rook relocated to destination");
  QWC_EQ_I(st.delta.edit_count, 4, "four square edits");
  QWC_EQ_I((int)((king == W_KING) ? pos.wKingSq : pos.bKingSq), (int)kto, "moving king cache updated");
  QWC_EQ_I((int)((king == W_KING) ? pos.bKingSq : pos.wKingSq), other_home, "other king square preserved");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "caches/key == rebuild");
  QWC_TRUE(pos_validate(&pos, &why), "result validates (safe)");
  QWC_TRUE(fen_emit(&pos, buf, (int)sizeof buf) == 1, "emit after");
  QWC_TRUE(strcmp(buf, after) == 0, "exact resulting FEN");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &base), "unmake restores exactly");
  return qwc_end();
}

static int t_exact(void) {
  int fail = 0;
  /* White kingside: e1->g1, h1->f1. */
  fail |= t_exact_one("castle.W_kingside", FEN_BEFORE_W,
                      mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE),
                      FILE_E,RANK_1,FILE_H,RANK_1, FILE_G,FILE_F, W_KING,W_ROOK,
                      "r3k2r/8/8/8/8/8/8/R4RK1 b kq - 8 20");
  /* White queenside: e1->c1, a1->d1. */
  fail |= t_exact_one("castle.W_queenside", FEN_BEFORE_W,
                      mvf(FILE_E,RANK_1,FILE_A,RANK_1,MV_CASTLE,NO_PIECE),
                      FILE_E,RANK_1,FILE_A,RANK_1, FILE_C,FILE_D, W_KING,W_ROOK,
                      "r3k2r/8/8/8/8/8/8/2KR3R b kq - 8 20");
  /* Black kingside: e8->g8, h8->f8. */
  fail |= t_exact_one("castle.B_kingside", FEN_BEFORE_B,
                      mvf(FILE_E,RANK_8,FILE_H,RANK_8,MV_CASTLE,NO_PIECE),
                      FILE_E,RANK_8,FILE_H,RANK_8, FILE_G,FILE_F, B_KING,B_ROOK,
                      "r4rk1/8/8/8/8/8/8/R3K2R w KQ - 8 21");
  /* Black queenside: e8->c8, a8->d8. */
  fail |= t_exact_one("castle.B_queenside", FEN_BEFORE_B,
                      mvf(FILE_E,RANK_8,FILE_A,RANK_8,MV_CASTLE,NO_PIECE),
                      FILE_E,RANK_8,FILE_A,RANK_8, FILE_C,FILE_D, B_KING,B_ROOK,
                      "2kr3r/8/8/8/8/8/8/R3K2R w KQ - 8 21");
  return fail;
}

/* Nested: white castles, then black castles. FENs round-trip; LIFO unmake
 * restores the original board exactly (kings, rooks, rights, ep, counters, key). */
static int t_seq(void) {
  qwc_begin("castle.seq_W_then_B");
  Position pos; StateInfo s0, s1; const char *why = NULL;
  memset(&s0, 0, sizeof s0); memset(&s1, 0, sizeof s1);
  char buf[FEN_BUF_SIZE];
  QWC_TRUE(fen_load(&pos, FEN_BEFORE_W, &why) == 1, "load before");
  Position base = pos;
  QWC_TRUE(fen_emit(&pos, buf, (int)sizeof buf) == 1 && strcmp(buf, FEN_BEFORE_W) == 0, "before FEN round-trips");
  QWC_TRUE(pos_make_move(&pos, &s0, mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE)) == 1, "W e1h1");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "after W: caches/key == rebuild");
  char mid[FEN_BUF_SIZE];
  QWC_TRUE(fen_emit(&pos, mid, (int)sizeof mid) == 1, "emit mid");
  Position midpos;
  QWC_TRUE(fen_load(&midpos, mid, &why) == 1, "mid FEN loads");
  QWC_TRUE(pos_equal(&midpos, &pos), "mid FEN round-trips to same pos");
  QWC_TRUE(pos_make_move(&pos, &s1, mvf(FILE_E,RANK_8,FILE_A,RANK_8,MV_CASTLE,NO_PIECE)) == 1, "B e8a8");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "final: caches/key == rebuild");
  QWC_TRUE(pos_validate(&pos, &why), "final validates");
  QWC_TRUE(fen_emit(&pos, buf, (int)sizeof buf) == 1, "emit final");
  QWC_TRUE(strcmp(buf, "2kr3r/8/8/8/8/8/8/R4RK1 w - - 9 21") == 0, "final FEN exact");
  Position fpos;
  QWC_TRUE(fen_load(&fpos, buf, &why) == 1, "final FEN loads");
  QWC_TRUE(pos_equal(&fpos, &pos), "final FEN round-trips to same pos");
  pos_unmake_move(&pos, &s1);
  pos_unmake_move(&pos, &s0);
  QWC_TRUE(pos_equal(&pos, &base), "LIFO unmake restores exactly");
  return qwc_end();
}

/* The 8 named castling-right combinations: -, K, Q, KQ, k, q, kq, KQkq. On a
 * full home board a WHITE kingside castle needs CR_WK; only the values that set
 * it are accepted, and on acceptance both white rights clear while the black
 * rights (the opponent's) are preserved. */
static int t_rights_opponent(void) {
  qwc_begin("castle.rights_opponent");
  static const int crs[8] = { CR_ALL, CR_WK, CR_WQ, CR_WK|CR_WQ, CR_BK, CR_BQ, CR_BK|CR_BQ, 0 };
  for (int i = 0; i < 8; i++) {
    int cr = crs[i];
    Position pos = castle_board(WHITE, cr, 7, 20);
    StateInfo st; memset(&st, 0, sizeof st);
    Move m = mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE);
    const char *why = NULL;
    if (cr & CR_WK) {
      QWC_TRUE(pos_make_move(&pos, &st, m) == 1, "accepted (right present)");
      QWC_EQ_I((int)pos.cr, cr & ~(CR_WK|CR_WQ), "white KQ cleared, black preserved");
      QWC_TRUE(derived_match_rebuild(&pos, &why), "key/caches == rebuild");
      pos_unmake_move(&pos, &st);
      QWC_EQ_I((int)pos.cr, cr, "unmake restores cr");
    } else {
      QWC_TRUE(pos_make_move(&pos, &st, m) == 0, "rejected (right missing)");
      QWC_EQ_I((int)pos.cr, cr, "cr unchanged on reject");
    }
  }
  return qwc_end();
}
/* All 16 rights states: the key (including the castling-rights contribution)
 * must match an independent rebuild after the make, and unmake must restore the
 * Position byte-for-byte (key included). */
static int t_rights_keys(void) {
  qwc_begin("castle.rights_16");
  for (int cr = 0; cr < 16; cr++) {
    Position pos = castle_board(WHITE, cr, 7, 20);
    Position base = pos;
    StateInfo st; memset(&st, 0, sizeof st);
    Move m = mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE);
    const char *why = NULL;
    if (cr & CR_WK) {
      QWC_TRUE(pos_make_move(&pos, &st, m) == 1, "accepted");
      QWC_TRUE(derived_match_rebuild(&pos, &why), "key == rebuild");
      pos_unmake_move(&pos, &st);
    } else {
      QWC_TRUE(pos_make_move(&pos, &st, m) == 0, "rejected");
    }
    QWC_TRUE(pos_equal(&pos, &base), "pos restored (cr/key/edits)");
  }
  return qwc_end();
}

/* Each rejected make must leave the Position and an initialized StateInfo
 * byte-for-byte unchanged. The FEN cases use the standard board; the directly
 * built cases fabricate configurations fen_load would itself refuse. */
static int t_reject(void) {
  int fail = 0;
  /* Missing the corresponding right (king + home rook are correctly in place). */
  fail |= reject_mkpos_case("castle.rej.no_WK", WHITE, CR_WQ, 7, 20, NO_SQUARE, std6, 6,
                            mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE));
  fail |= reject_mkpos_case("castle.rej.no_WQ", WHITE, CR_WK, 7, 20, NO_SQUARE, std6, 6,
                            mvf(FILE_E,RANK_1,FILE_A,RANK_1,MV_CASTLE,NO_PIECE));
  /* Friendly rook present at the destination but the right is absent. */
  static const PC rok_nor[3] = { {4,W_KING},{7,W_ROOK},{60,B_KING} };
  fail |= reject_mkpos_case("castle.rej.rook_no_right", WHITE, 0, 7, 20, NO_SQUARE, rok_nor, 3,
                            mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE));
  /* King absent / moved from the origin while the right is still claimed. */
  static const PC kng_moved[4] = { {3,W_KING},{7,W_ROOK},{0,W_ROOK},{60,B_KING} };
  fail |= reject_mkpos_case("castle.rej.king_moved", WHITE, CR_WK, 7, 20, NO_SQUARE, kng_moved, 4,
                            mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE));
  /* Home rook absent (f1 empty, no rook on h1) while the right is present. */
  static const PC no_rook[4] = { {4,W_KING},{0,W_ROOK},{60,B_KING},{56,B_ROOK} };
  fail |= reject_mkpos_case("castle.rej.rook_absent", WHITE, CR_WK, 7, 20, NO_SQUARE, no_rook, 4,
                            mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE));
  /* Opponent to move: the encoded move is the other side's. */
  fail |= reject_fen_case("castle.rej.wrong_side", FEN_BEFORE_B,
                          mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE));
  /* An enemy king sits on the (empty) king destination. */
  static const PC eking[3] = { {4,W_KING},{7,W_ROOK},{6,B_KING} };   /* black king on g1 */
  fail |= reject_mkpos_case("castle.rej.eking_dest", WHITE, CR_WK, 7, 20, NO_SQUARE, eking, 3,
                            mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE));
  /* Each required path square blocked (kingside f/g, queenside b/c/d), both
   * colors. The blocker is the opponent's pawn; only that square is occupied. */
  PC blk[7];
#define BLK(tag, side, bfs, mov) do { \
    for (int _i = 0; _i < 6; _i++) blk[_i] = std6[_i]; \
    blk[6].sq = bfs; blk[6].pc = (side == WHITE) ? B_PAWN : W_PAWN; \
    fail |= reject_mkpos_case(tag, side, CR_ALL, 7, 20, NO_SQUARE, blk, 7, mov); \
  } while (0)
  BLK("castle.rej.block_f1",  WHITE, sq_of(FILE_F,RANK_1), mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE));
  BLK("castle.rej.block_g1",  WHITE, sq_of(FILE_G,RANK_1), mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE));
  BLK("castle.rej.block_b1",  WHITE, sq_of(FILE_B,RANK_1), mvf(FILE_E,RANK_1,FILE_A,RANK_1,MV_CASTLE,NO_PIECE));
  BLK("castle.rej.block_c1",  WHITE, sq_of(FILE_C,RANK_1), mvf(FILE_E,RANK_1,FILE_A,RANK_1,MV_CASTLE,NO_PIECE));
  BLK("castle.rej.block_d1",  WHITE, sq_of(FILE_D,RANK_1), mvf(FILE_E,RANK_1,FILE_A,RANK_1,MV_CASTLE,NO_PIECE));
  BLK("castle.rej.block_f8",  BLACK, sq_of(FILE_F,RANK_8), mvf(FILE_E,RANK_8,FILE_H,RANK_8,MV_CASTLE,NO_PIECE));
  BLK("castle.rej.block_g8",  BLACK, sq_of(FILE_G,RANK_8), mvf(FILE_E,RANK_8,FILE_H,RANK_8,MV_CASTLE,NO_PIECE));
  BLK("castle.rej.block_b8",  BLACK, sq_of(FILE_B,RANK_8), mvf(FILE_E,RANK_8,FILE_A,RANK_8,MV_CASTLE,NO_PIECE));
  BLK("castle.rej.block_c8",  BLACK, sq_of(FILE_C,RANK_8), mvf(FILE_E,RANK_8,FILE_A,RANK_8,MV_CASTLE,NO_PIECE));
  BLK("castle.rej.block_d8",  BLACK, sq_of(FILE_D,RANK_8), mvf(FILE_E,RANK_8,FILE_A,RANK_8,MV_CASTLE,NO_PIECE));
#undef BLK
  /* Wrong rook square / non-orthodox spellings / king-to-king: all reject. */
  fail |= reject_fen_case("castle.rej.to_f1",  FEN_BEFORE_W, mvf(FILE_E,RANK_1,FILE_F,RANK_1,MV_CASTLE,NO_PIECE));
  fail |= reject_fen_case("castle.rej.e1g1",   FEN_BEFORE_W, mvf(FILE_E,RANK_1,FILE_G,RANK_1,MV_CASTLE,NO_PIECE));
  fail |= reject_fen_case("castle.rej.e1c1",   FEN_BEFORE_W, mvf(FILE_E,RANK_1,FILE_C,RANK_1,MV_CASTLE,NO_PIECE));
  fail |= reject_fen_case("castle.rej.a1h1",   FEN_BEFORE_W, mvf(FILE_A,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE));
  fail |= reject_fen_case("castle.rej.h1e1",   FEN_BEFORE_W, mvf(FILE_H,RANK_1,FILE_E,RANK_1,MV_CASTLE,NO_PIECE));
  fail |= reject_fen_case("castle.rej.d1e1",   FEN_BEFORE_W, mvf(FILE_D,RANK_1,FILE_E,RANK_1,MV_CASTLE,NO_PIECE));
  fail |= reject_fen_case("castle.rej.e8g8",   FEN_BEFORE_B, mvf(FILE_E,RANK_8,FILE_G,RANK_8,MV_CASTLE,NO_PIECE));
  fail |= reject_fen_case("castle.rej.a8h8",   FEN_BEFORE_B, mvf(FILE_A,RANK_8,FILE_H,RANK_8,MV_CASTLE,NO_PIECE));
  /* Mixed flags and a nonzero promotion payload. */
  fail |= reject_fen_case("castle.rej.flags_ep",    FEN_BEFORE_W, mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_EP|MV_CASTLE,NO_PIECE));
  fail |= reject_fen_case("castle.rej.flags_promo", FEN_BEFORE_W, mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_PROMO|MV_CASTLE,NO_PIECE));
  fail |= reject_fen_case("castle.rej.promo",       FEN_BEFORE_W, mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,QUEEN));
  /* from == to. */
  fail |= reject_fen_case("castle.rej.samesq",      FEN_BEFORE_W, mvf(FILE_E,RANK_1,FILE_E,RANK_1,MV_CASTLE,NO_PIECE));
  return fail;
}

/* Counter boundary fixtures. A quiet castle increments the halfmove clock and,
 * for Black, the fullmove. At the u16 ceiling the increment overflows and the
 * make is refused, leaving the Position unchanged. */
static int t_counters(void) {
  qwc_begin("castle.counters");
  Position pos; StateInfo st; Position base; const char *why = NULL;

  /* 1. White e1h1 from halfmove 65534 -> accepted, 65534 -> 65535. */
  pos = castle_board(WHITE, CR_ALL, 65534, 20); base = pos;
  memset(&st, 0, sizeof st);
  QWC_TRUE(pos_make_move(&pos, &st, mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE)) == 1, "W e1h1 hm65534 accepted");
  QWC_EQ_I((int)pos.halfmove, 65535, "halfmove 65534 -> 65535");
  QWC_EQ_I((int)pos.fullmove, 20, "white fullmove unchanged");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &base), "restored (1)");

  /* 2. White e1h1 from halfmove 65535 -> rejected (overflow). */
  pos = castle_board(WHITE, CR_ALL, 65535, 20); base = pos;
  memset(&st, 0, sizeof st);
  StateInfo st0 = st;
  QWC_TRUE(pos_make_move(&pos, &st, mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE)) == 0, "W e1h1 hm65535 rejected");
  QWC_TRUE(pos_equal(&pos, &base), "pos unchanged (2)");
  QWC_TRUE(state_equal(&st, &st0), "state unchanged (2)");

  /* 3. Black e8a8 from fullmove 65534 -> accepted, 65534 -> 65535. */
  pos = castle_board(BLACK, CR_ALL, 7, 65534); base = pos;
  memset(&st, 0, sizeof st);
  QWC_TRUE(pos_make_move(&pos, &st, mvf(FILE_E,RANK_8,FILE_A,RANK_8,MV_CASTLE,NO_PIECE)) == 1, "B e8a8 fm65534 accepted");
  QWC_EQ_I((int)pos.fullmove, 65535, "fullmove 65534 -> 65535");
  QWC_EQ_I((int)pos.halfmove, 8, "halfmove 7 -> 8 (quiet)");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &base), "restored (3)");

  /* 4. Black e8a8 from fullmove 65535 -> rejected (overflow). */
  pos = castle_board(BLACK, CR_ALL, 7, 65535); base = pos;
  memset(&st, 0, sizeof st);
  st0 = st;
  QWC_TRUE(pos_make_move(&pos, &st, mvf(FILE_E,RANK_8,FILE_A,RANK_8,MV_CASTLE,NO_PIECE)) == 0, "B e8a8 fm65535 rejected");
  QWC_TRUE(pos_equal(&pos, &base), "pos unchanged (4)");
  QWC_TRUE(state_equal(&st, &st0), "state unchanged (4)");

  /* 5. White e1h1 at fullmove 65535 -> accepted (white does not increment it). */
  pos = castle_board(WHITE, CR_ALL, 7, 65535); base = pos;
  memset(&st, 0, sizeof st);
  QWC_TRUE(pos_make_move(&pos, &st, mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE)) == 1, "W e1h1 fm65535 accepted");
  QWC_EQ_I((int)pos.fullmove, 65535, "white fullmove stays 65535");
  QWC_EQ_I((int)pos.halfmove, 8, "halfmove 7 -> 8");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &base), "restored (5)");
  (void)why;
  return qwc_end();
}

/* The recorded en-passant target must be cleared by a quiet castle. These three
 * boards carry a stale EP target (a just-double-pushed enemy pawn two moves ago)
 * with a white king that can castle kingside. After e1h1 the raw EP square must
 * be NO_SQUARE and the halfmove 0 -> 1; unmake restores the raw square and the
 * full board. */
static int t_epclear(void) {
  qwc_begin("castle.epclearing");
  static const char *fens[3] = {
    "4k3/8/8/3pP3/8/8/8/4K2R w K d6 0 20",
    "4k3/8/8/3p4/8/8/8/4K2R w K d6 0 20",
    "k3r3/8/8/3pP3/8/8/8/4K2R w K d6 0 20",
  };
  Square d6 = sq_of(FILE_D, RANK_6);
  for (int i = 0; i < 3; i++) {
    Position pos; StateInfo st; const char *why = NULL;
    memset(&st, 0, sizeof st);
    QWC_TRUE(fen_load(&pos, fens[i], &why) == 1, "load");
    Position base = pos;
    QWC_EQ_I((int)pos.ep_sq, (int)d6, "before: raw ep = d6");
    QWC_EQ_I((int)pos.halfmove, 0, "before: halfmove 0");
    QWC_TRUE(pos_make_move(&pos, &st, mvf(FILE_E,RANK_1,FILE_H,RANK_1,MV_CASTLE,NO_PIECE)) == 1, "W e1h1 accepted");
    QWC_EQ_I((int)pos.ep_sq, (int)NO_SQUARE, "after: raw ep cleared");
    QWC_EQ_I((int)pos.halfmove, 1, "after: halfmove 0 -> 1");
    QWC_TRUE(derived_match_rebuild(&pos, &why), "after: caches/key == rebuild");
    pos_unmake_move(&pos, &st);
    QWC_EQ_I((int)pos.ep_sq, (int)d6, "undo: raw ep restored to d6");
    QWC_TRUE(pos_equal(&pos, &base), "undo: full restore");
  }
  return qwc_end();
}

/* ---- checkpoint-3 completion: attacked-square + reserved-bit coverage ------
 *
 * The groups above verified the STRUCTURAL contract (pieces, right, encoding,
 * empty path) and the exact restore, but not two requested corners. This adds:
 *
 * (a) castles whose origin / transit / destination is ATTACKED (or whose rook
 *     origin is). These are still STRUCTURALLY valid -- the right is present, the
 *     pieces are on their home squares, the path is empty -- so the make must
 *     succeed and undo exactly. King safety (FIDE 3.8.2) is NOT an application
 *     precondition; it is movegen's job in T005, and for castling it must check
 *     the king's origin, transit AND destination squares (a final-position
 *     make/check/unmake alone misses castling out of / through check). Each of
 *     the five fixtures is tested together with its rank-reflect / color-swap
 *     mirror, so both colors are covered. We verify the derived caches/key
 *     against a rebuild and the exact full-state restore on unmake; we do NOT
 *     fen_emit the RESULT, because a castle that exposes the king leaves the
 *     non-mover in check, which the (correct) FEN validator rejects.
 *
 * (b) atomic rejection of a castling encoding that sets bit 15, or any of the
 *     high reserved bits 19..31 (pos + state byte-for-byte unchanged).
 */

/* Flip a color (WHITE=0 / BLACK=1). */
static Color cother(Color c) { return (Color)(1 - (int)c); }

/* Flip a piece's color. W_<p> and B_<p> differ by 8 (bit 3: W 1..6 / B 9..14). */
static Piece mirror_piece(Piece p) { return (Piece)(p ^ 8); }

/* Rank-reflect a square (file kept, rank 0<->7, 1<->6, ...). */
static Square mirror_sq(Square s) {
  return square_of((Square)file_of(s), (Square)(RANK_NB - 1 - rank_of(s)));
}

/* Rank-reflect + color-swap a Position into the mirror board (white rank-1
 * castling <-> black rank-8 castling). Each piece moves to the rank-reflected
 * square with its color flipped; the side and castling rights flip; the recorded
 * ep rank-reflects; the counters carry over. Rebuilds the derived caches. */
static void mirror_pos(const Position *in, Position *out) {
  pos_reset(out);
  for (int s = 0; s < SQ_NB; s++) {
    Piece p = in->mailbox[s];
    if (p == NO_PIECE)
      continue;
    out->mailbox[mirror_sq((Square)s)] = mirror_piece(p);   /* flip the color   */
  }
  out->side     = cother(in->side);
  out->cr       = (u8)(((in->cr & 3) << 2) | ((in->cr & 12) >> 2));  /* W-kq <-> B-KQ */
  out->ep_sq    = in->ep_sq == NO_SQUARE ? NO_SQUARE : mirror_sq(in->ep_sq);
  out->halfmove = in->halfmove;
  out->fullmove = in->fullmove;
  pos_rebuild(out);
}

/* One attacked-square fixture (and its rank-reflect / color-swap mirror): both
 * the original and the mirrored castle are structurally valid, so both make and
 * undo exactly. The moving king must land on its destination, the rook on its,
 * the derived caches/key must match a rebuild, and the full state (board,
 * counters, key) must be restored on unmake. */
static int t_attacked_one(const char *tag, const char *fen, Move m) {
  qwc_begin(tag);
  Position pos; StateInfo st; const char *why = NULL;
  QWC_TRUE(fen_load(&pos, fen, &why) == 1, "fixture loads");
  Position base = pos;
  Square rfrom = move_to(m);
  Color side = pos.side;
  Piece king = (side == WHITE) ? W_KING : B_KING;
  Piece rook = (side == WHITE) ? W_ROOK : B_ROOK;
  int home = (side == WHITE) ? RANK_1 : RANK_8;
  int kingside = (file_of(rfrom) == FILE_H);
  Square king_dest = square_of((Square)(kingside ? FILE_G : FILE_C), (Square)home);
  Square rook_dest = square_of((Square)(kingside ? FILE_F : FILE_D), (Square)home);

  /* the original */
  memset(&st, 0, sizeof st);
  QWC_TRUE(pos_make_move(&pos, &st, m) == 1, "structurally valid castle accepted");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "after: derived/key == rebuild");
  QWC_EQ_I((int)pos.mailbox[king_dest], (int)king, "king relocated to destination");
  QWC_EQ_I((int)pos.mailbox[rook_dest], (int)rook, "rook relocated to destination");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &base), "original: unmake restores exactly");

  /* the rank-reflect / color-swap mirror */
  Position mpos; mirror_pos(&base, &mpos);
  Position mbase = mpos;
  Move mm = make_move(mirror_sq(move_from(m)), mirror_sq(rfrom), MV_CASTLE, 0);
  memset(&st, 0, sizeof st);
  QWC_TRUE(pos_make_move(&mpos, &st, mm) == 1, "mirrored castle accepted");
  QWC_TRUE(derived_match_rebuild(&mpos, &why), "mirror after: derived/key == rebuild");
  Square mking_dest = mirror_sq(king_dest);
  Square mrook_dest = mirror_sq(rook_dest);
  QWC_EQ_I((int)mpos.mailbox[mking_dest], (int)mirror_piece(king), "mirror king relocated");
  QWC_EQ_I((int)mpos.mailbox[mrook_dest], (int)mirror_piece(rook), "mirror rook relocated");
  pos_unmake_move(&mpos, &st);
  QWC_TRUE(pos_equal(&mpos, &mbase), "mirror: unmake restores exactly");
  return qwc_end();
}

static int t_attacked(void) {
  int fail = 0;
  Move e1h1 = mvf(FILE_E, RANK_1, FILE_H, RANK_1, MV_CASTLE, NO_PIECE);
  Move e1a1 = mvf(FILE_E, RANK_1, FILE_A, RANK_1, MV_CASTLE, NO_PIECE);
  fail |= t_attacked_one("castle.attk_e1_atk", "k3r3/8/8/8/8/8/8/4K2R w K - 7 20", e1h1);  /* origin attacked */
  fail |= t_attacked_one("castle.attk_f1_atk", "k4r2/8/8/8/8/8/8/4K2R w K - 7 20", e1h1);  /* transit f1 attacked */
  fail |= t_attacked_one("castle.attk_g1_atk", "k5r1/8/8/8/8/8/8/4K2R w K - 7 20", e1h1);  /* transit g1 attacked */
  fail |= t_attacked_one("castle.attk_b1_atk", "1r5k/8/8/8/8/8/8/R3K3 w Q - 7 20", e1a1); /* rook transit b1 (legal) */
  fail |= t_attacked_one("castle.attk_h1_atk", "k6r/8/8/8/8/8/8/4K2R w K - 7 20", e1h1);  /* rook origin h1 (legal) */
  return fail;
}

/* A castling encoding that sets bit 15, or any of the high reserved bits
 * 19..31, must be rejected atomically (pos + state byte-for-byte unchanged),
 * before the make path would otherwise apply it. */
static int t_reserved(void) {
  qwc_begin("castle.reserved_bits");
  const char *fen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 7 20";
  const char *why = NULL;
  Position pos;
  QWC_TRUE(fen_load(&pos, fen, &why) == 1, "fixture loads");
  Move base_mv = mvf(FILE_E, RANK_1, FILE_H, RANK_1, MV_CASTLE, NO_PIECE);
  assert_reject(&pos, (Move)(base_mv | (1u << 15)));   /* reserved bit 15      */
  assert_reject(&pos, (Move)(base_mv | (1u << 19)));   /* lowest high reserved  */
  assert_reject(&pos, (Move)(base_mv | (1u << 20)));   /* a high reserved bit   */
  assert_reject(&pos, (Move)(base_mv | (1u << 31)));   /* the MSB (sign)        */
  return qwc_end();
}

int main(void) {
  int fail = 0;
  fail |= t_sizes();
  fail |= t_exact();
  fail |= t_seq();
  fail |= t_rights_opponent();
  fail |= t_rights_keys();
  fail |= t_reject();
  fail |= t_counters();
  fail |= t_epclear();
  fail |= t_attacked();
  fail |= t_reserved();
  printf("castling_makeunmake_test: %s\n", fail ? "FAILED" : "OK");
  return fail ? 1 : 0;
}
