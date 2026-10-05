/* QwenChess T004 checkpoint-1 test: reversible make/unmake.
 *
 * Exercises the one-Position / caller-StateInfo contract for ordinary moves:
 * a 3-move sequence with a StateInfo stack (raw EP records that clear, the
 * exact final FEN, full restoration on LIFO unmake), the a1a8 castling-rights
 * sequence (board + exact FEN), a double push creating legal / pinned / no-captor
 * en-passant targets (canonical file 3/-1/3, raw square retained, incremental
 * key == from-scratch rebuild), counter overflow bounds, and a malformed-move
 * suite verifying every reject is atomic (position + state byte-for-byte
 * unchanged). All checks are always active; the harness reports aggregates.
 */
#include "position/position.h"
#include "position/state.h"
#include "position/fen.h"
#include "core/types.h"
#include "core/square.h"
#include "core/bitboard.h"
#include "test_util.h"
#include <string.h>

/* ---- small helpers -------------------------------------------------------- */

static Square sq_of(int f, int r) { return square_of((Square)f, (Square)r); }
static Move mv(int f, int r, int tf, int tr) { return make_move(sq_of(f, r), sq_of(tf, tr), 0u, 0u); }
static Move mvf(int f, int r, int tf, int tr, int fl, int pr) { return make_move(sq_of(f, r), sq_of(tf, tr), (u32)fl, (u32)pr); }

/* Field-by-field equality over the entire Position. */
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

/* Field-by-field equality over the logical content of the undo record. */
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
 * ep_sq, side, cr and counters untouched), so this confirms the incremental
 * make() produced exactly what a full rebuild would -- including the key. */
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

/* Build a fresh Position from an explicit (small) piece list, then rebuild the
 * derived caches. Used to assemble the minimal malformed / special positions. */
static Position mkpos(Color side, int cr, int hm, int fm, const PC *pcs, int n) {
  Position p; pos_reset(&p);
  p.side = side; p.cr = (u8)cr; p.halfmove = (HalfMoveClock)hm; p.fullmove = (FullMoveNumber)fm; p.ep_sq = NO_SQUARE;
  for (int i = 0; i < n; i++)
    p.mailbox[pcs[i].sq] = pcs[i].pc;
  pos_rebuild(&p);
  return p;
}
/* Reset a Position to empty with the given scalar fields (no ep). */
static void newpos(Position *p, Color side, int cr, int hm, int fm) {
  pos_reset(p);
  p->side = side; p->cr = (u8)cr; p->halfmove = (HalfMoveClock)hm; p->fullmove = (FullMoveNumber)fm; p->ep_sq = NO_SQUARE;
}
/* Place one piece directly in the mailbox (call pos_rebuild() after all). */
static void place(Position *p, Square sq, int pc) { p->mailbox[sq] = (Piece)pc; }

/* make() must succeed. */
static void check_accept(Position *pos, StateInfo *st, Move m, const char *name) {
  QWC_TRUE(pos_make_move(pos, st, m) == 1, name);
}
/* make() must reject, leaving both the Position and the StateInfo untouched. */
static void check_reject(Position *pos, StateInfo *st, Move m, const char *name) {
  Position pb = *pos;
  StateInfo sb = *st;
  QWC_TRUE(pos_make_move(pos, st, m) == 0, name);
  QWC_TRUE(pos_equal(pos, &pb), name);
  QWC_TRUE(state_equal(st, &sb), name);
}

/* ---- a 3-move sequence with a StateInfo stack ----------------------------- */
static int t_sequence(void) {
  qwc_begin("makeunmake.seq_e4c5nf3");
  Position pos;
  StateInfo st[3];
  memset(st, 0, sizeof st);
  const char *why = NULL;
  QWC_TRUE(fen_load(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &why) == 1, "load startpos");
  Position base = pos;

  /* 1.e4 (white double push) */
  check_accept(&pos, &st[0], mv(4, 1, 4, 3), "1.e4 accepted");
  QWC_EQ_I((int)pos.side, (int)BLACK, "side -> black after e4");
  QWC_EQ_I((int)pos.ep_sq, (int)sq_of(4, 2), "raw ep = e3 after e4");
  QWC_EQ_I(pos_canon_ep_file(&pos), -1, "e3 canonical file -1 (no black captor)");
  QWC_EQ_I((int)pos.halfmove, 0, "halfmove reset by e4");
  QWC_EQ_I((int)pos.fullmove, 1, "fullmove 1 after white move");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "e4 derived/key == rebuild");
  QWC_TRUE(pos_validate(&pos, &why), "post-e4 validates (valid-but-uncapturable ep)");

  /* 1...c5 (black double push) */
  check_accept(&pos, &st[1], mv(2, 6, 2, 4), "1...c5 accepted");
  QWC_EQ_I((int)pos.ep_sq, (int)sq_of(2, 5), "raw ep = c6 after c5");
  QWC_EQ_I(pos_canon_ep_file(&pos), -1, "c6 canonical file -1 (no white captor)");
  QWC_EQ_I((int)pos.halfmove, 0, "halfmove reset by c5");
  QWC_EQ_I((int)pos.fullmove, 2, "fullmove 2 after black move");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "c5 derived/key == rebuild");

  /* 2.Nf3 (quiet knight) -- the recorded ep clears */
  check_accept(&pos, &st[2], mv(6, 0, 5, 2), "2.Nf3 accepted");
  QWC_EQ_I((int)pos.ep_sq, (int)NO_SQUARE, "ep clears after non-push Nf3");
  QWC_EQ_I(pos_canon_ep_file(&pos), -1, "no ep -> canonical -1");
  QWC_EQ_I((int)pos.side, (int)BLACK, "side -> black after Nf3");
  QWC_EQ_I((int)pos.halfmove, 1, "halfmove 1 after quiet Nf3");
  QWC_EQ_I((int)pos.fullmove, 2, "fullmove stays 2 after white move");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "final derived/key == rebuild");
  QWC_TRUE(pos_validate(&pos, &why), "final validates");

  /* exact final FEN (the stated target) */
  char fbuf[FEN_BUF_SIZE];
  QWC_TRUE(fen_emit(&pos, fbuf, (int)sizeof fbuf) == 1, "emit final fen");
  QWC_TRUE(strcmp(fbuf, "rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2") == 0, "final fen exact");

  /* LIFO unmake of all three -> full restoration */
  pos_unmake_move(&pos, &st[2]);
  pos_unmake_move(&pos, &st[1]);
  pos_unmake_move(&pos, &st[0]);
  QWC_TRUE(pos_equal(&pos, &base), "reverse unmake restores startpos exactly");
  return qwc_end();
}

/* ---- the a1a8 castling-rights sequence ------------------------------------ */
static int t_a1a8(void) {
  qwc_begin("makeunmake.a1a8_castling");
  Position pos;
  StateInfo st;
  memset(&st, 0, sizeof st);
  const char *why = NULL;
  QWC_TRUE(fen_load(&pos, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 12 42", &why) == 1, "load a1a8 start");
  Position base = pos;

  check_accept(&pos, &st, mv(0, 0, 0, 7), "Ra1xa8 accepted");   /* a1 -> a8 */
  QWC_EQ_I((int)pos.mailbox[sq_of(0, 7)], (int)W_ROOK, "white rook now on a8");
  QWC_EQ_I((int)pos.mailbox[sq_of(0, 0)], (int)NO_PIECE, "a1 now empty");
  QWC_EQ_I((int)pos.mailbox[sq_of(4, 7)], (int)B_KING, "black king e8");
  QWC_EQ_I((int)pos.mailbox[sq_of(7, 7)], (int)B_ROOK, "black rook h8");
  QWC_EQ_I((int)pos.mailbox[sq_of(4, 0)], (int)W_KING, "white king e1");
  QWC_EQ_I((int)pos.mailbox[sq_of(7, 0)], (int)W_ROOK, "white rook h1");
  QWC_EQ_I((int)pos.wKingSq, (int)sq_of(4, 0), "wKingSq e1");
  QWC_EQ_I((int)pos.bKingSq, (int)sq_of(4, 7), "bKingSq e8");
  QWC_EQ_I((int)pos.cr, (int)(CR_WK | CR_BK), "castling Kk after rook move + capture");
  QWC_EQ_I((int)pos.ep_sq, (int)NO_SQUARE, "no ep");
  QWC_EQ_I((int)pos.halfmove, 0, "halfmove reset by capture");
  QWC_EQ_I((int)pos.fullmove, 42, "fullmove unchanged (white moved)");
  QWC_EQ_I((int)pos.side, (int)BLACK, "side -> black");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "derived/key == rebuild");
  QWC_TRUE(pos_validate(&pos, &why), "post-move validates");

  char fbuf[FEN_BUF_SIZE];
  QWC_TRUE(fen_emit(&pos, fbuf, (int)sizeof fbuf) == 1, "emit fen");
  QWC_TRUE(strcmp(fbuf, "R3k2r/8/8/8/8/8/8/4K2R b Kk - 0 42") == 0, "a1a8 fen exact");

  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &base), "a1a8 unmake restores exactly (cr/halfmove/key)");
  return qwc_end();
}

/* ---- double push -> en-passant target (legal / pinned / no-captor) -------- */
static int t_ep_doublepush(void) {
  qwc_begin("makeunmake.ep_doublepush");
  StateInfo st;
  const char *why = NULL;
  /* Each: black pawn on d7, black to move, no ep, hm 9 fm 19; play d7d5. The
   * pushed pawn lands on d5 recording ep d6; the canonical file is 3 (legal
   * captor), -1 (pinned captor) and 3 (captor on the other file). */
  const PC g1[] = { { sq_of(4,0), W_KING }, { sq_of(4,7), B_KING }, { sq_of(3,6), B_PAWN }, { sq_of(4,4), W_PAWN } };
  const PC g2[] = { { sq_of(4,0), W_KING }, { sq_of(0,7), B_KING }, { sq_of(4,7), B_ROOK }, { sq_of(3,6), B_PAWN }, { sq_of(4,4), W_PAWN } };
  const PC g5[] = { { sq_of(4,0), W_KING }, { sq_of(4,7), B_KING }, { sq_of(3,6), B_PAWN }, { sq_of(2,4), W_PAWN } };
  const PC *geos[] = { g1, g2, g5 };
  const int  npcs[] = { 4, 5, 4 };
  const int  exp[]  = { 3, -1, 3 };
  const char *nm[]  = { "geo#1 captor king-safe -> file 3",
                        "geo#2 pinned captor -> file -1",
                        "geo#5 captor other side -> file 3" };

  for (int gi = 0; gi < 3; gi++) {
    Position pos = mkpos(BLACK, 0, 9, 19, geos[gi], npcs[gi]);
    memset(&st, 0, sizeof st);
    QWC_TRUE(pos_validate(&pos, &why), "pre-move validates");
    check_accept(&pos, &st, mv(3, 6, 3, 4), "d7d5 (black double push) accepted");
    QWC_EQ_I((int)pos.ep_sq, (int)sq_of(3, 5), "raw ep = d6 retained");
    QWC_EQ_I(pos_canon_ep_file(&pos), exp[gi], nm[gi]);
    QWC_EQ_I((int)pos.side, (int)WHITE, "side -> white after black push");
    QWC_EQ_I((int)pos.halfmove, 0, "halfmove reset by pawn push");
    QWC_EQ_I((int)pos.fullmove, 20, "fullmove 20 after black move");
    QWC_TRUE(derived_match_rebuild(&pos, &why), "derived/key == rebuild");
    QWC_TRUE(pos_validate(&pos, &why), "post-move validates");
    char fbuf[FEN_BUF_SIZE];
    if (fen_emit(&pos, fbuf, (int)sizeof fbuf) == 1)
      printf("    evidence: %s\n", fbuf);   /* evidence only, not pass/fail */
    pos_unmake_move(&pos, &st);
    QWC_EQ_I((int)pos.ep_sq, (int)NO_SQUARE, "unmake clears the ep record");
    QWC_EQ_I((int)pos.halfmove, 9, "unmake restores halfmove");
    QWC_EQ_I((int)pos.fullmove, 19, "unmake restores fullmove");
  }
  return qwc_end();
}

/* ---- rule-50 / fullmove counter bounds across make ------------------------ */
static int t_counters(void) {
  qwc_begin("makeunmake.counters");
  StateInfo st;

  /* quiet move (knight) crossing 100 and 255 increments; 65534 -> 65535 is the
   * legal maximum; 65535 + a quiet move would overflow and is rejected. */
  {
    Position p; pos_set_startpos(&p); p.halfmove = 100;
    memset(&st, 0, sizeof st);
    check_accept(&p, &st, mv(1, 0, 2, 2), "Nb1c3 quiet from hm 100");   /* b1 -> c3 */
    QWC_EQ_I((int)p.halfmove, 101, "halfmove 100 -> 101");
    pos_unmake_move(&p, &st);
    QWC_EQ_I((int)p.halfmove, 100, "unmake restores halfmove 100");
  }
  {
    Position p; pos_set_startpos(&p); p.halfmove = 255;
    memset(&st, 0, sizeof st);
    check_accept(&p, &st, mv(1, 0, 2, 2), "Nb1c3 quiet from hm 255");
    QWC_EQ_I((int)p.halfmove, 256, "halfmove 255 -> 256");
    pos_unmake_move(&p, &st);
    QWC_EQ_I((int)p.halfmove, 255, "unmake restores halfmove 255");
  }
  {
    Position p; pos_set_startpos(&p); p.halfmove = 65534;
    memset(&st, 0, sizeof st);
    check_accept(&p, &st, mv(1, 0, 2, 2), "Nb1c3 quiet from hm 65534");
    QWC_EQ_I((int)p.halfmove, 65535, "halfmove 65534 -> 65535 (max)");
    pos_unmake_move(&p, &st);
    QWC_EQ_I((int)p.halfmove, 65534, "unmake restores halfmove 65534");
  }
  {
    Position p; pos_set_startpos(&p); p.halfmove = 65535;
    memset(&st, 0, sizeof st);
    check_reject(&p, &st, mv(1, 0, 2, 2), "hm 65535 + quiet rejected (would overflow)");
  }
  /* a pawn move and a capture each reset the clock to 0 */
  {
    Position p; pos_set_startpos(&p); p.halfmove = 100;
    memset(&st, 0, sizeof st);
    check_accept(&p, &st, mv(4, 1, 4, 2), "e2e3 pawn push");
    QWC_EQ_I((int)p.halfmove, 0, "pawn move resets halfmove to 0");
  }
  {
    Position p; pos_reset(&p);
    p.mailbox[sq_of(0, 0)] = W_ROOK;   /* a1 */
    p.mailbox[sq_of(0, 1)] = B_PAWN;   /* a2: capturable by the a1 rook */
    p.mailbox[sq_of(4, 0)] = W_KING;   /* e1 */
    p.mailbox[sq_of(4, 7)] = B_KING;   /* e8 */
    p.side = WHITE; p.cr = 0; p.ep_sq = NO_SQUARE;
    p.halfmove = 100; p.fullmove = 1;
    pos_rebuild(&p);
    memset(&st, 0, sizeof st);
    check_accept(&p, &st, mv(0, 0, 0, 1), "Ra1xa2 capture");
    QWC_EQ_I((int)p.halfmove, 0, "capture resets halfmove to 0");
  }
  /* fullmove 65535 + a black move would overflow -> rejected */
  {
    Position p; pos_set_startpos(&p);
    StateInfo s1; memset(&s1, 0, sizeof s1);
    check_accept(&p, &s1, mv(4, 1, 4, 3), "e2e4 (white) -> black to move");
    p.fullmove = 65535;
    StateInfo s2; memset(&s2, 0, sizeof s2);
    check_reject(&p, &s2, mv(4, 6, 4, 4), "fm 65535 + black e7e5 rejected (would overflow)");
  }
  /* fullmove 65535 + a white move stays 65535 (legal) */
  {
    Position p; pos_set_startpos(&p); p.fullmove = 65535;
    memset(&st, 0, sizeof st);
    check_accept(&p, &st, mv(4, 1, 4, 3), "e2e4 (white) from fm 65535");
    QWC_EQ_I((int)p.fullmove, 65535, "white move does not increment fullmove");
  }
  return qwc_end();
}

/* ---- malformed / unsupported moves: every reject must be atomic ----------- */
static int t_malformed(void) {
  qwc_begin("makeunmake.malformed");
  Position pos;
  StateInfo st;
  memset(&st, 0, sizeof st);

  /* encoding-level rejects (plausible from/to, bad bits) on the start position */
  pos_set_startpos(&pos);
  check_reject(&pos, &st, 0, "null/zero move");
  check_reject(&pos, &st, mv(4, 0, 4, 0), "from == to (e1e1)");
  check_reject(&pos, &st, (Move)(mv(4, 1, 4, 2) | (1u << 15)), "reserved bit 15 set");
  check_reject(&pos, &st, (Move)(mv(4, 1, 4, 2) | (1u << 19)), "reserved bits 19..31 set");
  /* e1->g1 with MV_CASTLE is the king-to-king spelling, which is non-orthodox
   * under the frozen king-origin-to-rook-origin encoding (and the path is
   * blocked on the start position); checkpoint 3 rejects it on the destination. */
  check_reject(&pos, &st, mvf(4, 0, 6, 0, MV_CASTLE, NO_PIECE), "castle e1g1 (non-orthodox destination, not a rook origin)");
  check_reject(&pos, &st, mv(0, 6, 0, 5), "source holds a black piece while white to move (a7)");
  check_reject(&pos, &st, mv(0, 0, 1, 0), "friendly destination (a1 rook -> b1 knight)");

  /* promotions and underpromotions are now supported (checkpoint 4): a7a8=Q and
   * a7a8=R are valid straight promotions (the a7 pawn reaches the last rank on an
   * empty square). Each is accepted, its promoted piece verified on a8, and then
   * undone so the MV_EP check below still sees the original board. (The dedicated
   * promotion_makeunmake_test.c verifies the full family. The MV_EP e5xd6 below
   * is still rejected: this position records NO en-passant target for d6, and a
   * valid EP capture must target the recorded square -- checkpoint 2.) */
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, sq_of(0, 6), W_PAWN);   /* a7 */
  place(&pos, sq_of(4, 0), W_KING);   /* e1 */
  place(&pos, sq_of(4, 7), B_KING);   /* e8 */
  place(&pos, sq_of(3, 4), B_PAWN);   /* d5: an e5xd6 ep is geometrically real */
  place(&pos, sq_of(4, 4), W_PAWN);   /* e5 */
  pos_rebuild(&pos);
  {
    Position pbefore = pos;
    check_accept(&pos, &st, mvf(0, 6, 0, 7, MV_PROMO, QUEEN), "promotion a7a8=Q");
    QWC_EQ_I((int)pos.mailbox[sq_of(0, 7)], (int)W_QUEEN, "a8 holds the promoted queen");
    pos_unmake_move(&pos, &st);
    QWC_TRUE(pos_equal(&pos, &pbefore), "a7a8=Q undone exactly");
    check_accept(&pos, &st, mvf(0, 6, 0, 7, MV_PROMO, ROOK), "underpromotion a7a8=R");
    QWC_EQ_I((int)pos.mailbox[sq_of(0, 7)], (int)W_ROOK, "a8 holds the promoted rook");
    pos_unmake_move(&pos, &st);
    QWC_TRUE(pos_equal(&pos, &pbefore), "a7a8=R undone exactly");
  }
  check_reject(&pos, &st, mvf(4, 4, 3, 5, MV_EP, NO_PIECE), "MV_EP e5xd6 with no recorded ep target");

  /* a knight to a non-attack square (manhattan 4) and to a same-colour square */
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, sq_of(0, 0), W_KNIGHT);  /* a1 */
  place(&pos, sq_of(2, 0), W_KING);    /* c1 */
  place(&pos, sq_of(0, 7), B_KING);    /* a8 */
  pos_rebuild(&pos);
  check_reject(&pos, &st, mv(0, 0, 4, 0), "knight a1->e1 (manhattan 4, not an attack)");
  check_reject(&pos, &st, mv(0, 0, 1, 1), "knight a1->b2 (same colour, not an attack)");

  /* blocked sliders: a blocker one square on, the target two squares away */
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, sq_of(0, 0), W_QUEEN);   /* a1 */
  place(&pos, sq_of(0, 1), W_PAWN);    /* a2: on the a-file */
  place(&pos, sq_of(2, 0), W_KING);    /* c1 */
  place(&pos, sq_of(0, 7), B_KING);    /* a8 */
  pos_rebuild(&pos);
  check_reject(&pos, &st, mv(0, 0, 0, 2), "queen a1->a3 blocked by the a2 pawn");
  place(&pos, sq_of(0, 1), NO_PIECE);  /* clear the a-file pawn      */
  place(&pos, sq_of(0, 0), W_BISHOP);  /* a1: a bishop               */
  place(&pos, sq_of(1, 1), W_PAWN);    /* b2: on the a1-h8 diagonal  */
  pos_rebuild(&pos);
  check_reject(&pos, &st, mv(0, 0, 2, 2), "bishop a1->c3 blocked by the b2 pawn");
  place(&pos, sq_of(1, 1), NO_PIECE);  /* clear the diagonal pawn    */
  place(&pos, sq_of(0, 0), W_ROOK);    /* a1: a rook                 */
  place(&pos, sq_of(0, 1), W_PAWN);    /* a2: back on the a-file     */
  pos_rebuild(&pos);
  check_reject(&pos, &st, mv(0, 0, 0, 2), "rook a1->a3 blocked by the a2 pawn");

  /* a king two squares away is not adjacent */
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, sq_of(4, 0), W_KING);    /* e1 */
  place(&pos, sq_of(2, 7), B_KING);    /* c8 */
  pos_rebuild(&pos);
  check_reject(&pos, &st, mv(4, 0, 4, 2), "king e1->e3 (two ranks away)");

  /* capturing the enemy king is never allowed */
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, sq_of(0, 0), W_QUEEN);   /* a1 */
  place(&pos, sq_of(0, 2), B_KING);    /* a3: on the a-file */
  place(&pos, sq_of(2, 0), W_KING);    /* c1 */
  pos_rebuild(&pos);
  check_reject(&pos, &st, mv(0, 0, 0, 2), "queen captures the enemy king");

  /* a pawn push into an occupied square */
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, sq_of(4, 1), W_PAWN);    /* e2 */
  place(&pos, sq_of(4, 2), B_PAWN);    /* e3: in the way */
  place(&pos, sq_of(0, 0), W_KING);    /* a1 */
  place(&pos, sq_of(0, 7), B_KING);    /* a8 */
  pos_rebuild(&pos);
  check_reject(&pos, &st, mv(4, 1, 4, 2), "pawn push e2->e3 against an occupied square");

  /* a pawn cannot move backward */
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, sq_of(4, 3), W_PAWN);    /* e4 */
  place(&pos, sq_of(0, 0), W_KING);    /* a1 */
  place(&pos, sq_of(0, 7), B_KING);    /* a8 */
  pos_rebuild(&pos);
  check_reject(&pos, &st, mv(4, 3, 4, 2), "pawn backward e4->e3");

  /* a double push is only from the starting rank, and only with a clear path */
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, sq_of(4, 2), W_PAWN);    /* e3: not a starting rank */
  place(&pos, sq_of(0, 0), W_KING);    /* a1 */
  place(&pos, sq_of(0, 7), B_KING);    /* a8 */
  pos_rebuild(&pos);
  check_reject(&pos, &st, mv(4, 2, 4, 4), "double push from the wrong rank (e3->e5)");
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, sq_of(4, 1), W_PAWN);    /* e2 */
  place(&pos, sq_of(4, 2), B_PAWN);    /* e3: occupied intermediate */
  place(&pos, sq_of(0, 0), W_KING);    /* a1 */
  place(&pos, sq_of(0, 7), B_KING);    /* a8 */
  pos_rebuild(&pos);
  check_reject(&pos, &st, mv(4, 1, 4, 3), "double push e2->e4 with an occupied intermediate");

  return qwc_end();
}

int main(void) {
  int fail = 0;
  fail |= t_sequence();
  fail |= t_a1a8();
  fail |= t_ep_doublepush();
  fail |= t_counters();
  fail |= t_malformed();
  return fail ? 1 : 0;
}
