/* QwenChess: T004 checkpoint 4 -- PROMOTION / underpromotion make & unmake.
 *
 * Dedicated to the promotion move kind. The general make/unmake contract is in
 * makeunmake_test.c; the reversible architecture in state.c. A promotion
 * reverses through TWO square edits (the pawn leaves its origin; the destination
 * -- empty for a straight promo, the victim for a capture-promo -- receives the
 * promoted piece of the mover's color). The payload is the target PieceType
 * (KNIGHT..QUEEN = 2..5); the board piece is payload + 8*side.
 *
 * Like the other make/unmake checkpoints, promotion application is STRUCTURAL,
 * not legal: a structurally valid promotion whose promoted piece does or does
 * not give check is applied and reversed; own-king safety is movegen's job in
 * T005 ("giving check with the promoted piece is valid").
 *
 * Coverage (one group per requested corner):
 *   t_straight  -- every file, all four types, both colors.
 *   t_capture   -- a diagonal capture-promo onto every enemy piece type, both
 *                  colors, incl. the a-/h-file edges (no file wrap); the
 *                  king-capture rejection.
 *   t_castling  -- the castling-rights transition when a promo captures a1/h1/
 *                  a8/h8; a straight promo leaves rights alone.
 *   t_reject    -- every atomic rejection: geometry, payload, mover, flags, and
 *                  the reserved bits.
  *   t_sequence  -- a7a8=Q through a8a7 (the canonical undo): full state + key.
  *   t_null      -- the null move remains rejected.
  *   t_epclear   -- a promo is not a double push: the recorded ep target is
  *                  cleared (and its canonical key file removed), for a
  *                  capturable EP and a recorded-but-uncapturable one.
  *   t_nested    -- a capture-promo then a capture of the promoted piece, and
  *                  two promotions in a row; complete LIFO restoration.
  *   t_pinned    -- a structurally valid promo that leaves the mover's own king
  *                  in check (applied, not validated); plus its mirror.
  *   t_counters  -- halfmove 65535 -> 0, black fullmove 65534 -> 65535, black
  *                  65535 rejected, white 65535 accepted.
  */
#include "position/position.h"
#include "position/state.h"
#include "position/fen.h"
#include "core/types.h"
#include "core/square.h"
#include "core/bitboard.h"
#include "test_util.h"
#include <string.h>

/* ---- structural-comparison helpers (never shipped) ------------------------ */
static int pos_equal(const Position *a, const Position *b) {
  if (a->side != b->side || a->cr != b->cr || a->ep_sq != b->ep_sq ||
      a->halfmove != b->halfmove || a->fullmove != b->fullmove ||
      a->wKingSq != b->wKingSq || a->bKingSq != b->bKingSq || a->key != b->key)
    return 0;
  for (int p = 0; p < PIECE_NB; p++) if (a->byPiece[p] != b->byPiece[p]) return 0;
  for (int c = 0; c < COLOR_NB; c++) if (a->byColor[c] != b->byColor[c]) return 0;
  if (a->occ != b->occ) return 0;
  for (int s = 0; s < SQ_NB; s++) if (a->mailbox[s] != b->mailbox[s]) return 0;
  return 1;
}
static int state_equal(const StateInfo *a, const StateInfo *b) {
  if (a->prev_side != b->prev_side || a->prev_cr != b->prev_cr || a->prev_ep != b->prev_ep ||
      a->prev_halfmove != b->prev_halfmove || a->prev_fullmove != b->prev_fullmove ||
      a->prev_key != b->prev_key || a->delta.edit_count != b->delta.edit_count)
    return 0;
  for (int i = 0; i < MAX_EDIT; i++) {
    if (a->delta.edits[i].sq != b->delta.edits[i].sq ||
        a->delta.edits[i].before != b->delta.edits[i].before ||
        a->delta.edits[i].after != b->delta.edits[i].after)
      return 0;
  }
  return 1;
}
static int derived_match_rebuild(Position *pos, const char **why) {
  Position c = *pos;
  pos_rebuild(&c);
  if (pos_validate(&c, why) != 1) return 0;
  for (int p = 0; p < PIECE_NB; p++) if (pos->byPiece[p] != c.byPiece[p]) return 0;
  for (int col = 0; col < COLOR_NB; col++) if (pos->byColor[col] != c.byColor[col]) return 0;
  if (pos->occ != c.occ || pos->wKingSq != c.wKingSq || pos->bKingSq != c.bKingSq || pos->key != c.key) return 0;
  return 1;
}
/* Like derived_match_rebuild but WITHOUT requiring the board to validate -- for a
 * structurally applied but king-unsafe result (e.g. a pinned promotion) whose
 * unsafe child the FEN validator would refuse. The derived caches + key must
 * still match a from-scratch rebuild of the same (unsafe) board. */
static int derived_match_raw(const Position *pos, const char **why) {
  Position c = *pos;
  pos_rebuild(&c);
  for (int p = 0; p < PIECE_NB; p++) if (pos->byPiece[p] != c.byPiece[p]) { if (why) *why = "byPiece mismatch vs rebuild"; return 0; }
  for (int col = 0; col < COLOR_NB; col++) if (pos->byColor[col] != c.byColor[col]) { if (why) *why = "byColor mismatch vs rebuild"; return 0; }
  if (pos->occ != c.occ)          { if (why) *why = "occ mismatch vs rebuild"; return 0; }
  if (pos->wKingSq != c.wKingSq || pos->bKingSq != c.bKingSq) { if (why) *why = "king square mismatch vs rebuild"; return 0; }
  if (pos->key != c.key)          { if (why) *why = "key mismatch vs rebuild"; return 0; }
  return 1;
}
static Color cother(Color c) { return (Color)(1 - (int)c); }

/* ---- small position builder ------------------------------------------------ */
static void newpos(Position *p, Color side, u8 cr, u16 hm, u16 fm) {
  pos_reset(p);
  p->side = side; p->cr = cr; p->halfmove = hm; p->fullmove = fm; p->ep_sq = NO_SQUARE;
  pos_rebuild(p);
}
static void place(Position *p, Square sq, int piece) {
  p->mailbox[sq] = (Piece)piece;
  pos_rebuild(p);
}
static Square s(int f, int r) { return square_of((Square)f, (Square)r); }

/* A promotion that MUST apply. Verifies the board result (promoted piece on the
 * destination, pawn gone, captured victim gone, kings untouched), the counters
 * (side flips, halfmove -> 0, fullmove +1 for black), the derived caches + key
 * against a rebuild, the castling rights when a rook is captured (cr_after == -1
 * skips that check), an optional FEN round-trip (when the result is a valid
 * position), and the exact full-state restore on unmake. pos_unmake_move leaves
 * the caller's StateInfo slot intact for LIFO reuse, so the POSITION -- not the
 * slot -- is what must come back exact. */
static void expect_promo(Position *pos, StateInfo *st, Move m, int cr_after,
                         int expect_valid, const char *ctx) {
  Position base = *pos;
  memset(st, 0, sizeof *st);
  Square from = move_from(m), to = move_to(m);
  Color side = pos->side;
  Piece promoted = (Piece)(move_promo(m) + (side == BLACK ? 8 : 0));
  Piece victim = pos->mailbox[to];
  Square wk = pos->wKingSq, bk = pos->bKingSq;
  int nfm = (side == BLACK) ? (int)pos->fullmove + 1 : (int)pos->fullmove;

  QWC_TRUE(pos_make_move(pos, st, m) == 1, ctx);
  QWC_EQ_I((int)pos->mailbox[to], promoted, "promoted piece lands on destination");
  QWC_EQ_I((int)pos->mailbox[from], 0, "pawn removed from origin");
  if (victim != NO_PIECE)
    QWC_TRUE((pos->byPiece[victim] & square_bb(to)) == 0, "captured victim removed");
  QWC_EQ_I((int)pos->wKingSq, (int)wk, "white king square unchanged");
  QWC_EQ_I((int)pos->bKingSq, (int)bk, "black king square unchanged");
  QWC_EQ_I((int)pos->side, (int)cother(side), "side flips");
  QWC_EQ_I((int)pos->halfmove, 0, "halfmove resets to 0");
  QWC_EQ_I((int)pos->fullmove, nfm, "fullmove unchanged (white) / +1 (black)");
  if (cr_after >= 0)
    QWC_EQ_I((int)pos->cr, cr_after, "castling rights after the promo");
  QWC_TRUE(derived_match_rebuild(pos, NULL), "derived caches + key match a rebuild");

  if (expect_valid) {
    char buf[FEN_BUF_SIZE];
    QWC_TRUE(pos_validate(pos, NULL) == 1, "result validates (FEN precondition)");
    QWC_TRUE(fen_emit(pos, buf, (int)sizeof buf) == 1, "result FEN emits");
    Position p2;
    QWC_TRUE(fen_load(&p2, buf, NULL) == 1, "result FEN round-trips");
    QWC_TRUE(pos_equal(pos, &p2), "reloaded FEN equals the made position");
  }

  pos_unmake_move(pos, st);
  QWC_TRUE(pos_equal(pos, &base), "unmake restores the whole Position");
}

/* A move that MUST be rejected atomically (pos + state byte-for-byte unchanged,
 * so the slot can be reused). */
static void assert_reject(Position *pos, StateInfo *st, Move m, const char *name) {
  Position pb = *pos;
  memset(st, 0, sizeof *st);
  StateInfo sb = *st;
  QWC_TRUE(pos_make_move(pos, st, m) == 0, name);
  QWC_TRUE(pos_equal(pos, &pb), "pos unchanged after rejection");
  QWC_TRUE(state_equal(st, &sb), "state unchanged after rejection");
}

/* ---- straight promotions: every file, all four types, both colors ---------- */
static int t_straight(void) {
  qwc_begin("promo.straight");
  for (int c = 0; c < 2; c++) {
    Color side = (Color)c;
    for (int f = FILE_A; f <= FILE_H; f++) {
      for (int pt = KNIGHT; pt <= QUEEN; pt++) {
        int penult = (side == WHITE) ? RANK_7 : RANK_2;
        int final  = (side == WHITE) ? RANK_8 : RANK_1;
        Position pos;
        newpos(&pos, side, 0, 0, 1);
        place(&pos, s(f, penult), (side == WHITE) ? W_PAWN : B_PAWN);
        place(&pos, s(FILE_E, (side == WHITE) ? RANK_1 : RANK_3), (side == WHITE) ? W_KING : B_KING);
        place(&pos, s(FILE_E, (side == WHITE) ? RANK_6 : RANK_8), (side == WHITE) ? B_KING : W_KING);
        Square from = s(f, penult), to = s(f, final);
        char ctx[48];
        snprintf(ctx, sizeof ctx, "%s %c%d->%c%d=%c",
                 (side == WHITE ? "w" : "b"), "abcdefgh"[f], "12345678"[penult],
                 "abcdefgh"[f], "12345678"[final], "NBRQ"[pt - KNIGHT]);
        StateInfo st;
        expect_promo(&pos, &st, make_move(from, to, MV_PROMO, (u32)pt), -1, 1, ctx);
      }
    }
  }
  return qwc_end();
}

/* ---- capture promotions: a one-file diagonal onto every enemy piece type --- */
static int t_capture(void) {
  qwc_begin("promo.capture");
  const int enemy_types[4] = { KNIGHT, BISHOP, ROOK, QUEEN };
  for (int c = 0; c < 2; c++) {
    Color side = (Color)c;
    for (int t = 0; t < 4; t++) {
      int penult = (side == WHITE) ? RANK_7 : RANK_2;
      int final  = (side == WHITE) ? RANK_8 : RANK_1;
      Square from = s(FILE_B, penult);   /* b7 / b2   */
      Square to   = s(FILE_A, final);    /* a8 / a1: a one-file diagonal */
      int enemy   = enemy_types[t] + (side == WHITE ? 8 : 0);
      Position pos;
      newpos(&pos, side, 0, 0, 1);
      place(&pos, from, (side == WHITE) ? W_PAWN : B_PAWN);
      place(&pos, to, enemy);
      place(&pos, s(FILE_E, (side == WHITE) ? RANK_1 : RANK_3), (side == WHITE) ? W_KING : B_KING);
      place(&pos, s(FILE_E, (side == WHITE) ? RANK_6 : RANK_8), (side == WHITE) ? B_KING : W_KING);
      char ctx[48];
      snprintf(ctx, sizeof ctx, "%s b%dxa%d=Q (victim %c)",
               (side == WHITE ? "w" : "b"), penult + 1, final + 1, "NBRQ"[t]);
      StateInfo st;
      expect_promo(&pos, &st, make_move(from, to, MV_PROMO, QUEEN), -1, 1, ctx);
    }
  }
  /* edge files: the a-file and h-file diagonals must not wrap. white a7xb8 and
   * h7xg8; the black mirrors a2xb1 and h2xg1. */
  struct { int ff, tf; } edges[4] = { { FILE_A, FILE_B }, { FILE_H, FILE_G },
                                      { FILE_A, FILE_B }, { FILE_H, FILE_G } };
  for (int c = 0; c < 2; c++) {
    Color side = (Color)c;
    int penult = (side == WHITE) ? RANK_7 : RANK_2;
    int final  = (side == WHITE) ? RANK_8 : RANK_1;
    for (int e = 0; e < 2; e++) {
      Square from = s(edges[e].ff, penult);
      Square to   = s(edges[e].tf, final);
      int enemy   = QUEEN + (side == WHITE ? 8 : 0);
      Position pos;
      newpos(&pos, side, 0, 0, 1);
      place(&pos, from, (side == WHITE) ? W_PAWN : B_PAWN);
      place(&pos, to, enemy);
      place(&pos, s(FILE_E, (side == WHITE) ? RANK_1 : RANK_3), (side == WHITE) ? W_KING : B_KING);
      place(&pos, s(FILE_E, (side == WHITE) ? RANK_6 : RANK_8), (side == WHITE) ? B_KING : W_KING);
      char ctx[48];
      snprintf(ctx, sizeof ctx, "%s %c%dx%c%d=Q (edge, no wrap)",
               (side == WHITE ? "w" : "b"), "abcdefgh"[edges[e].ff], penult + 1,
               "abcdefgh"[edges[e].tf], final + 1);
      StateInfo st;
      expect_promo(&pos, &st, make_move(from, to, MV_PROMO, QUEEN), -1, 1, ctx);
    }
  }
  /* capturing a king is never legal, even with MV_PROMO: rejected atomically. */
  for (int c = 0; c < 2; c++) {
    Color side = (Color)c;
    int penult = (side == WHITE) ? RANK_7 : RANK_2;
    int final  = (side == WHITE) ? RANK_8 : RANK_1;
    Square from = s(FILE_B, penult), to = s(FILE_A, final);   /* the enemy king sits on the target */
    Position pos;
    newpos(&pos, side, 0, 0, 1);
    place(&pos, from, (side == WHITE) ? W_PAWN : B_PAWN);
    place(&pos, to, (side == WHITE) ? B_KING : W_KING);       /* the enemy king on the destination */
    place(&pos, s(FILE_E, (side == WHITE) ? RANK_1 : RANK_3), (side == WHITE) ? W_KING : B_KING);
    StateInfo st;
    char ctx[48];
    snprintf(ctx, sizeof ctx, "%s captures the king (rejected)", (side == WHITE ? "w" : "b"));
    assert_reject(&pos, &st, make_move(from, to, MV_PROMO, QUEEN), ctx);
  }
  return qwc_end();
}

/* ---- castling-rights transitions when a promo captures a home-square rook -- */
static int t_castling(void) {
  qwc_begin("promo.castling_rights");
  struct Case { Color side; int ff, tf; int promo; int expect_cr; const char *ctx; };
  const struct Case cases[5] = {
    { WHITE, FILE_B, FILE_A, QUEEN, CR_WK | CR_WQ | CR_BK, "w b7xa8=Q clears CR_BQ" },
    { WHITE, FILE_G, FILE_H, QUEEN, CR_WK | CR_WQ | CR_BQ, "w g7xh8=Q clears CR_BK" },
    { BLACK, FILE_B, FILE_A, QUEEN, CR_WK | CR_BK | CR_BQ, "b b2xa1=Q clears CR_WQ" },
    { BLACK, FILE_G, FILE_H, QUEEN, CR_WQ | CR_BK | CR_BQ, "b g2xh1=Q clears CR_WK" },
    { WHITE, FILE_C, FILE_C, QUEEN, CR_ALL,              "w c7c8=Q (straight) keeps rights" },
  };
  for (int i = 0; i < 5; i++) {
    const struct Case *cc = &cases[i];
    int penult = (cc->side == WHITE) ? RANK_7 : RANK_2;
    int final  = (cc->side == WHITE) ? RANK_8 : RANK_1;
    Square from = s(cc->ff, penult), to = s(cc->tf, final);
    Position pos;
    newpos(&pos, cc->side, CR_ALL, 0, 1);
    place(&pos, from, (cc->side == WHITE) ? W_PAWN : B_PAWN);
    /* all four home rooks, so CR_ALL is fully backed (v_cr_backing) before the
     * move; a capture removes exactly one, keeping the surviving rights backed.
     * (The straight case targets c8, which no rook occupies.) */
    place(&pos, s(FILE_A, RANK_1), W_ROOK);   /* a1 : backs CR_WQ */
    place(&pos, s(FILE_H, RANK_1), W_ROOK);   /* h1 : backs CR_WK */
    place(&pos, s(FILE_A, RANK_8), B_ROOK);   /* a8 : backs CR_BQ */
    place(&pos, s(FILE_H, RANK_8), B_ROOK);   /* h8 : backs CR_BK */
    place(&pos, s(FILE_E, RANK_1), W_KING);
    place(&pos, s(FILE_E, RANK_8), B_KING);
    StateInfo st;
    expect_promo(&pos, &st, make_move(from, to, MV_PROMO, (u32)cc->promo), cc->expect_cr, 1, cc->ctx);
  }
  return qwc_end();
}

/* A reject-test base: white to move, no rights, a white pawn on a7, kings on
 * e1/e8. Callers add/override pieces for the specific illegal move under test. */
static void base7(Position *p) {
  newpos(p, WHITE, 0, 0, 1);
  place(p, s(FILE_A, RANK_7), W_PAWN);
  place(p, s(FILE_E, RANK_1), W_KING);
  place(p, s(FILE_E, RANK_8), B_KING);
}

/* ---- every atomic rejection the make path must honor ----------------------- */
static int t_reject(void) {
  qwc_begin("promo.reject");
  Position pos; StateInfo st;
  Square a7 = s(FILE_A, RANK_7), a8 = s(FILE_A, RANK_8), b8 = s(FILE_B, RANK_8),
         c8 = s(FILE_C, RANK_8), a6 = s(FILE_A, RANK_6);

  base7(&pos); place(&pos, a8, B_ROOK);   /* straight onto an occupied square   */
  assert_reject(&pos, &st, make_move(a7, a8, MV_PROMO, QUEEN), "straight onto an occupied square");
  base7(&pos);                              /* b8 is empty                        */
  assert_reject(&pos, &st, make_move(a7, b8, MV_PROMO, QUEEN), "diagonal onto an empty square");
  base7(&pos); place(&pos, b8, W_ROOK);    /* the diagonal target holds a friend */
  assert_reject(&pos, &st, make_move(a7, b8, MV_PROMO, QUEEN), "diagonal captures a friendly piece");
  base7(&pos); place(&pos, b8, B_KING);   /* capture the (single) king on the target */
  assert_reject(&pos, &st, make_move(a7, b8, MV_PROMO, QUEEN), "diagonal captures a king");
  base7(&pos);                              /* backward / not the final rank       */
  assert_reject(&pos, &st, make_move(a7, a6, MV_PROMO, QUEEN), "backward (not the final rank)");
  base7(&pos); pos.mailbox[a7] = NO_PIECE; pos_rebuild(&pos);   /* pawn a6 instead of a7 */
  place(&pos, a6, W_PAWN);
  assert_reject(&pos, &st, make_move(a6, a7, MV_PROMO, QUEEN), "origin not on the penultimate rank");

  base7(&pos);
  assert_reject(&pos, &st, make_move(a7, a8, MV_PROMO, PAWN), "PAWN payload is not a legal target");
  base7(&pos);
  assert_reject(&pos, &st, make_move(a7, a8, MV_PROMO, KING), "KING payload is not a legal target");
  base7(&pos);
  assert_reject(&pos, &st, make_move(a7, a8, MV_PROMO, NO_PIECE_TYPE), "zero payload is not a legal target");
  base7(&pos);
  assert_reject(&pos, &st, make_move(a7, a8, MV_PROMO, 7u), "a 7 payload is not a legal target");

  base7(&pos); pos.mailbox[a7] = NO_PIECE; pos_rebuild(&pos);   /* a knight, not a pawn, on a7 */
  place(&pos, a7, W_KNIGHT);
  assert_reject(&pos, &st, make_move(a7, a8, MV_PROMO, QUEEN), "MV_PROMO with a non-pawn mover");
  base7(&pos); pos.mailbox[a7] = NO_PIECE; pos_rebuild(&pos);   /* the enemy pawn on a7        */
  place(&pos, a7, B_PAWN);
  assert_reject(&pos, &st, make_move(a7, a8, MV_PROMO, QUEEN), "MV_PROMO with the enemy pawn");
  base7(&pos);
  assert_reject(&pos, &st, make_move(a7, a8, 0u, QUEEN), "promo payload on a non-promotion move");

  base7(&pos);
  assert_reject(&pos, &st, make_move(a7, a8, MV_EP | MV_PROMO, QUEEN), "mixed EP|PROMO flags");
  base7(&pos);
  assert_reject(&pos, &st, make_move(a7, a8, MV_CASTLE | MV_PROMO, QUEEN), "mixed CASTLE|PROMO flags");
  base7(&pos);
  assert_reject(&pos, &st, (Move)(make_move(a7, a8, MV_PROMO, QUEEN) | (1u << 15)), "reserved bit 15 set");
  base7(&pos);
  assert_reject(&pos, &st, (Move)(make_move(a7, a8, MV_PROMO, QUEEN) | (1u << 20)), "a high reserved bit set");
  base7(&pos); place(&pos, c8, B_QUEEN);   /* a two-file jump: neither push nor diagonal */
  assert_reject(&pos, &st, make_move(a7, c8, MV_PROMO, QUEEN), "a two-file (non-pawn) displacement");
  base7(&pos);                              /* a7->a8 is a final-rank push ...          */
  assert_reject(&pos, &st, make_move(a7, a8, 0u, 0u), "...but with no MV_PROMO flag it is not a promotion");
  base7(&pos);                              /* a7->h8 spans seven files: a wrap would need (file+1)&7 */
  assert_reject(&pos, &st, make_move(a7, s(FILE_H, RANK_8), MV_PROMO, QUEEN), "a file-wrapping displacement (a7->h8) is not a push or a single diagonal");
  return qwc_end();
}

/* ---- the canonical a7a8=Q make then a8a7 undo: full state + the undo record */
static int t_sequence(void) {
  qwc_begin("promo.sequence");
  Position pos;
  newpos(&pos, WHITE, CR_WK | CR_WQ, 7, 20);   /* a8 empty, so a7a8=Q is a straight promo */
  place(&pos, s(FILE_A, RANK_7), W_PAWN);
  place(&pos, s(FILE_A, RANK_1), W_ROOK);      /* backs CR_WQ */
  place(&pos, s(FILE_H, RANK_1), W_ROOK);      /* backs CR_WK */
  place(&pos, s(FILE_E, RANK_1), W_KING);
  place(&pos, s(FILE_E, RANK_8), B_KING);
  Position base = pos;
  StateInfo st;
  memset(&st, 0, sizeof st);
  Move promo = make_move(s(FILE_A, RANK_7), s(FILE_A, RANK_8), MV_PROMO, QUEEN);
  QWC_TRUE(pos_make_move(&pos, &st, promo) == 1, "a7a8=Q applies");
  QWC_EQ_I((int)st.prev_side, (int)WHITE, "undo record: prev side");
  QWC_EQ_I((int)st.prev_cr, (int)(CR_WK | CR_WQ), "undo record: prev castling");
  QWC_EQ_I((int)st.prev_ep, NO_SQUARE, "undo record: prev ep");
  QWC_EQ_I((int)st.prev_halfmove, 7, "undo record: prev halfmove");
  QWC_EQ_I((int)st.prev_fullmove, 20, "undo record: prev fullmove");
  QWC_EQ_I(st.delta.edit_count, 2, "delta has two edits");
  QWC_EQ_I((int)st.delta.edits[0].sq, (int)s(FILE_A, RANK_7), "edit 0 origin a7");
  QWC_EQ_I((int)st.delta.edits[0].before, (int)W_PAWN, "edit 0: a7 was the pawn");
  QWC_EQ_I((int)st.delta.edits[0].after, 0, "edit 0: a7 now empty");
  QWC_EQ_I((int)st.delta.edits[1].sq, (int)s(FILE_A, RANK_8), "edit 1 destination a8");
  QWC_EQ_I((int)st.delta.edits[1].before, 0, "edit 1: a8 was empty (straight)");
  QWC_EQ_I((int)st.delta.edits[1].after, (int)W_QUEEN, "edit 1: a8 now the queen");
  QWC_EQ_I((int)pos.halfmove, 0, "halfmove -> 0");
  QWC_EQ_I((int)pos.fullmove, 20, "fullmove unchanged (white)");
  QWC_EQ_I((int)pos.side, (int)BLACK, "side -> black");
  QWC_EQ_I((int)pos.cr, (int)(CR_WK | CR_WQ), "castling rights unchanged (straight promo)");
  QWC_TRUE(derived_match_rebuild(&pos, NULL), "derived + key match a rebuild");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &base), "a8a7 (unmake) restores the whole Position");
  return qwc_end();
}

/* ---- the null move remains a rejection, not a promotion -------------------- */
static int t_null(void) {
  qwc_begin("promo.null");
  Position pos;
  base7(&pos);
  StateInfo st;
  assert_reject(&pos, &st, move_none(), "the null move (0) is rejected");
  return qwc_end();
}

/* ---- counter boundaries -----------------------------------------------------
 * A promo is a pawn move, so the halfmove clock ALWAYS resets to 0 (a maxed clock
 * is a valid input, not an overflow); only a BLACK promo increments the fullmove
 * number, so a maxed black fullmove overflows and is rejected while a maxed white
 * one is accepted. Each case uses a straight a-file promo far from the kings. */
static void promo_base(Position *p, Color side, u16 hm, u16 fm) {
  newpos(p, side, 0, hm, fm);
  int penult = (side == WHITE) ? RANK_7 : RANK_2;
  place(p, s(FILE_A, penult), (side == WHITE) ? W_PAWN : B_PAWN);
  place(p, s(FILE_E, (side == WHITE) ? RANK_1 : RANK_3), (side == WHITE) ? W_KING : B_KING);
  place(p, s(FILE_E, (side == WHITE) ? RANK_6 : RANK_8), (side == WHITE) ? B_KING : W_KING);
}
static Move straight_promo(Color side) {
  int penult = (side == WHITE) ? RANK_7 : RANK_2;
  int fin    = (side == WHITE) ? RANK_8 : RANK_1;
  return make_move(s(FILE_A, penult), s(FILE_A, fin), MV_PROMO, QUEEN);
}
static int t_counters(void) {
  qwc_begin("promo.counters");
  Position pos; StateInfo st; Position base;
  promo_base(&pos, WHITE, 65535, 1); base = pos;
  memset(&st, 0, sizeof st);
  QWC_TRUE(pos_make_move(&pos, &st, straight_promo(WHITE)) == 1, "w hm65535 accepted (a promo resets the clock)");
  QWC_EQ_I((int)pos.halfmove, 0, "halfmove 65535 -> 0");
  QWC_EQ_I((int)pos.fullmove, 1, "white fullmove unchanged (1)");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &base), "restored (1)");
  promo_base(&pos, BLACK, 0, 65534); base = pos;
  memset(&st, 0, sizeof st);
  QWC_TRUE(pos_make_move(&pos, &st, straight_promo(BLACK)) == 1, "b fm65534 accepted");
  QWC_EQ_I((int)pos.fullmove, 65535, "fullmove 65534 -> 65535");
  QWC_EQ_I((int)pos.halfmove, 0, "halfmove -> 0");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &base), "restored (2)");
  promo_base(&pos, BLACK, 0, 65535); base = pos;
  memset(&st, 0, sizeof st); StateInfo st0 = st;
  QWC_TRUE(pos_make_move(&pos, &st, straight_promo(BLACK)) == 0, "b fm65535 rejected (a +1 would overflow)");
  QWC_TRUE(pos_equal(&pos, &base), "pos unchanged (3)");
  QWC_TRUE(state_equal(&st, &st0), "state unchanged (3)");
  promo_base(&pos, WHITE, 0, 65535); base = pos;
  memset(&st, 0, sizeof st);
  QWC_TRUE(pos_make_move(&pos, &st, straight_promo(WHITE)) == 1, "w fm65535 accepted (white does not increment it)");
  QWC_EQ_I((int)pos.fullmove, 65535, "white fullmove stays 65535");
  QWC_EQ_I((int)pos.halfmove, 0, "halfmove -> 0");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &base), "restored (4)");
  return qwc_end();
}

/* ---- a promotion is not a double push: it clears the RECORDED ep target -----
 * (raw ep -> NO_SQUARE) and the key loses any canonical-ep-file contribution;
 * unmake restores the exact raw record + key. "present" is a capturable EP whose
 * file is in the key; "absent" is a recorded-but-uncapturable EP (no key file). */
static int t_epclear(void) {
  qwc_begin("promo.epclear");
  const char *fens[2] = {
    "8/P3k3/8/3pP3/8/8/8/4K3 w - d6 0 1",  /* capturable: e5 pawn takes d6 -> FILE_D in key */
    "8/P3k3/8/3p4/8/8/8/4K3 w - d6 0 1",   /* recorded but uncapturable -> no key file        */
  };
  const int canon[2] = { (int)FILE_D, -1 };
  Square d6 = s(FILE_D, RANK_6);
  for (int i = 0; i < 2; i++) {
    Position pos; StateInfo st; const char *why = NULL; char buf[FEN_BUF_SIZE];
    QWC_TRUE(fen_load(&pos, fens[i], &why) == 1, "fixture loads");
    Position base = pos;
    QWC_EQ_I((int)pos.ep_sq, (int)d6, "before: raw recorded ep = d6");
    QWC_EQ_I(pos_canon_ep_file(&pos), canon[i], "before: canonical ep file (present vs absent)");
    memset(&st, 0, sizeof st);
    Move promo = make_move(s(FILE_A, RANK_7), s(FILE_A, RANK_8), MV_PROMO, QUEEN);
    QWC_TRUE(pos_make_move(&pos, &st, promo) == 1, "a7a8=Q applies (a promo is not a double push)");
    QWC_EQ_I((int)pos.ep_sq, (int)NO_SQUARE, "after: the raw ep target is cleared");
    QWC_EQ_I(pos_canon_ep_file(&pos), -1, "after: no canonical ep file");
    QWC_TRUE(derived_match_rebuild(&pos, &why), "derived caches + key == rebuild after");
    QWC_TRUE(fen_emit(&pos, buf, (int)sizeof buf) == 1, "after FEN emits");
    Position p2;
    QWC_TRUE(fen_load(&p2, buf, &why) == 1, "after FEN round-trips");
    QWC_TRUE(pos_equal(&pos, &p2), "reloaded FEN equals the made position");
    pos_unmake_move(&pos, &st);
    QWC_EQ_I((int)pos.ep_sq, (int)d6, "unmake restores the raw recorded ep");
    QWC_TRUE(pos_equal(&pos, &base), "unmake restores the whole Position (incl. key)");
  }
  return qwc_end();
}

/* ---- nested promotions / a promotion followed by capture of the promoted ----
 * piece, with complete LIFO restoration. Structural make (not legality) is the
 * point: both moves apply, both unmake in strict LIFO order, and the whole
 * Position (board, derived caches, counters, key) comes back exact. */
static int t_nested(void) {
  qwc_begin("promo.nested");
  Position pos; StateInfo s0, s1;
  /* A: a capture-promo creates a queen; the enemy king then captures it. */
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, s(FILE_A, RANK_7), W_PAWN);     /* a7 : the pawn that promotes  */
  place(&pos, s(FILE_B, RANK_8), B_ROOK);     /* b8 : taken by the promo      */
  place(&pos, s(FILE_C, RANK_8), B_KING);     /* c8 : adjacent, takes the queen */
  place(&pos, s(FILE_E, RANK_1), W_KING);
  Position base = pos;
  memset(&s0, 0, sizeof s0); memset(&s1, 0, sizeof s1);
  QWC_TRUE(pos_make_move(&pos, &s0, make_move(s(FILE_A, RANK_7), s(FILE_B, RANK_8), MV_PROMO, QUEEN)) == 1, "A: a7xb8=Q (capture-promo)");
  QWC_EQ_I((int)pos.mailbox[s(FILE_B, RANK_8)], (int)W_QUEEN, "A: the new white queen sits on b8");
  QWC_EQ_I((int)pos.mailbox[s(FILE_A, RANK_7)], 0, "A: the origin a7 is empty");
  QWC_TRUE(pos_make_move(&pos, &s1, make_move(s(FILE_C, RANK_8), s(FILE_B, RANK_8), 0, 0)) == 1, "A: black Kc8xb8 captures the promoted piece");
  QWC_EQ_I((int)pos.mailbox[s(FILE_B, RANK_8)], (int)B_KING, "A: the black king now occupies b8");
  QWC_EQ_I((int)pos.mailbox[s(FILE_C, RANK_8)], 0, "A: the origin c8 is empty");
  pos_unmake_move(&pos, &s1);
  pos_unmake_move(&pos, &s0);
  QWC_TRUE(pos_equal(&pos, &base), "A: full LIFO restore (pawn a7, rook b8, kings c8/e1)");
  /* B: two promotions in a row (white a7a8=Q, then black a2a1=Q). */
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, s(FILE_A, RANK_7), W_PAWN);
  place(&pos, s(FILE_A, RANK_2), B_PAWN);
  place(&pos, s(FILE_D, RANK_1), W_KING);     /* off the a-file */
  place(&pos, s(FILE_D, RANK_8), B_KING);
  Position base2 = pos;
  memset(&s0, 0, sizeof s0); memset(&s1, 0, sizeof s1);
  QWC_TRUE(pos_make_move(&pos, &s0, make_move(s(FILE_A, RANK_7), s(FILE_A, RANK_8), MV_PROMO, QUEEN)) == 1, "B: white a7a8=Q");
  QWC_EQ_I((int)pos.mailbox[s(FILE_A, RANK_8)], (int)W_QUEEN, "B: the white queen is on a8");
  QWC_TRUE(pos_make_move(&pos, &s1, make_move(s(FILE_A, RANK_2), s(FILE_A, RANK_1), MV_PROMO, QUEEN)) == 1, "B: black a2a1=Q");
  QWC_EQ_I((int)pos.mailbox[s(FILE_A, RANK_1)], (int)B_QUEEN, "B: the black queen is on a1");
  pos_unmake_move(&pos, &s1);
  pos_unmake_move(&pos, &s0);
  QWC_TRUE(pos_equal(&pos, &base2), "B: full LIFO restore (pawns a7/a2, kings d1/d8)");
  return qwc_end();
}

/* ---- a structurally valid promo that leaves the MOVER'S OWN king in check --
 * ("pinned"): the make path applies it (structural, not legal); T005's movegen
 * will later reject the unsafe move. We verify the board result + the derived
 * caches/key against a NON-validating rebuild, but deliberately do NOT run
 * pos_validate / fen_emit on the unsafe result. Tested with its rank-reflect /
 * color-swap mirror. */
static int t_pinned(void) {
  qwc_begin("promo.pinned");
  const char *why = NULL;
  /* original (white to move): a8 black rook + b8 black bishop (the diagonal
   * target) + h8 black king; a7 white pawn; a1 white king. a7xb8=Q empties a7,
   * unblocking the a8 rook's line to the a1 king -> the mover's own king ends in
   * check, so the result is unsafe (T005's job to reject, not this make path's). */
  Position pos;
  newpos(&pos, WHITE, 0, 0, 1);
  place(&pos, s(FILE_A, RANK_1), W_KING);
  place(&pos, s(FILE_A, RANK_7), W_PAWN);
  place(&pos, s(FILE_A, RANK_8), B_ROOK);
  place(&pos, s(FILE_B, RANK_8), B_BISHOP);
  place(&pos, s(FILE_H, RANK_8), B_KING);
  Position base = pos;
  StateInfo st; memset(&st, 0, sizeof st);
  QWC_TRUE(pos_make_move(&pos, &st, make_move(s(FILE_A, RANK_7), s(FILE_B, RANK_8), MV_PROMO, QUEEN)) == 1, "pinned a7xb8=Q applies structurally");
  QWC_EQ_I((int)pos.mailbox[s(FILE_B, RANK_8)], (int)W_QUEEN, "the promoted queen lands on b8");
  QWC_EQ_I((int)pos.mailbox[s(FILE_A, RANK_7)], 0, "the origin a7 is empty (opening the a-file)");
  QWC_EQ_I((int)pos.mailbox[s(FILE_A, RANK_8)], (int)B_ROOK, "the a8 rook survives, now pointing at the a1 king");
  QWC_TRUE(derived_match_raw(&pos, &why), "derived caches + key == rebuild (unsafe board)");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal(&pos, &base), "unmake restores the whole Position");
  /* mirror (rank-reflect + color-swap, black to move): a1 white rook + b1 white
   * bishop + h1 white king; a2 black pawn; a8 black king. a2xb1=Q empties a2,
   * unblocking the a1 rook's line to the a8 king -> the mover's (black) king in
   * check. */
  Position mp;
  newpos(&mp, BLACK, 0, 0, 1);
  place(&mp, s(FILE_A, RANK_8), B_KING);
  place(&mp, s(FILE_A, RANK_2), B_PAWN);
  place(&mp, s(FILE_A, RANK_1), W_ROOK);
  place(&mp, s(FILE_B, RANK_1), W_BISHOP);
  place(&mp, s(FILE_H, RANK_1), W_KING);
  Position mbase = mp;
  memset(&st, 0, sizeof st);
  QWC_TRUE(pos_make_move(&mp, &st, make_move(s(FILE_A, RANK_2), s(FILE_B, RANK_1), MV_PROMO, QUEEN)) == 1, "mirrored pinned a2xb1=Q applies structurally");
  QWC_EQ_I((int)mp.mailbox[s(FILE_B, RANK_1)], (int)B_QUEEN, "mirror: the promoted queen lands on b1");
  QWC_EQ_I((int)mp.mailbox[s(FILE_A, RANK_2)], 0, "mirror: the origin a2 is empty (opening the a-file)");
  QWC_TRUE(derived_match_raw(&mp, &why), "mirror: derived caches + key == rebuild (unsafe board)");
  pos_unmake_move(&mp, &st);
  QWC_TRUE(pos_equal(&mp, &mbase), "mirror: unmake restores the whole Position");
  (void)why;
  return qwc_end();
}

int main(void) {
  int fail = 0;
  fail |= t_straight();
  fail |= t_capture();
  fail |= t_castling();
  fail |= t_reject();
  fail |= t_sequence();
  fail |= t_null();
  fail |= t_epclear();
  fail |= t_nested();
  fail |= t_pinned();
  fail |= t_counters();
  printf("promotion_makeunmake_test: %s\n", fail ? "FAILED" : "OK");
  return fail ? 1 : 0;
}
