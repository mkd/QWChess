/* QwenChess T003 checkpoint-1 test: Position storage, startpos, invariants.
 *
 * Validates the one authoritative Position (mailbox + derived bitboards +
 * side/cr/ep/counters/key): the exact orthodox start position, a few explicitly
 * constructed no-EP positions, reset, isolation between instances, counter
 * bounds, key invariance to counters, the canonical-EP derivation, and a
 * field-at-a-time corruption suite (validator must reject WITHOUT mutating the
 * input). Checks are always active (never NDEBUG-guarded); the harness reports
 * aggregate counts and per-failure diagnostics.
 *
 * This is a representation-consistency check, not move legality / reachability.
 * FEN parsing, make/unmake and movegen are later checkpoints.
 */
#include "position/position.h"
#include "core/types.h"
#include "core/square.h"
#include "core/bitboard.h"
#include "core/zobrist.h"
#include "test_util.h"
#include <string.h>

/* Build a minimal kings-only position from an explicit mailbox. */
static void place(Position *pos, Square sq, Piece p) { pos->mailbox[sq] = p; }

/* Independent expected key: recompute from the raw snapshot via the verified
 * core hasher, never from the position's cached key/derivation. */
static u64 expected_key(const Position *pos, int ep_file) {
  return zobrist_compute(pos->mailbox, pos->side, pos->cr, ep_file);
}

/* Square from 0-indexed file/rank (test convenience). */
static Square sq_of(int f, int r) { return square_of((Square)f, (Square)r); }

/* ---- the exact orthodox start position ---- */
static int t_startpos(void) {
  qwc_begin("pos.startpos");
  Position p; pos_set_startpos(&p);
  for (int f = 0; f < FILE_NB; f++) {
    QWC_EQ_I((int)p.mailbox[8 + f],  W_PAWN, "white pawn rank");
    QWC_EQ_I((int)p.mailbox[48 + f], B_PAWN, "black pawn rank");
  }
  /* white back rank R N B Q K B N R (squares 0..7) */
  QWC_EQ_I((int)p.mailbox[0], W_ROOK,   "W a1"); QWC_EQ_I((int)p.mailbox[1], W_KNIGHT, "W b1");
  QWC_EQ_I((int)p.mailbox[2], W_BISHOP, "W c1"); QWC_EQ_I((int)p.mailbox[3], W_QUEEN,  "W d1");
  QWC_EQ_I((int)p.mailbox[4], W_KING,   "W e1"); QWC_EQ_I((int)p.mailbox[5], W_BISHOP, "W f1");
  QWC_EQ_I((int)p.mailbox[6], W_KNIGHT, "W g1"); QWC_EQ_I((int)p.mailbox[7], W_ROOK,   "W h1");
  /* black back rank r n b q k b n r (squares 56..63) */
  QWC_EQ_I((int)p.mailbox[56], B_ROOK,   "B a8"); QWC_EQ_I((int)p.mailbox[57], B_KNIGHT, "B b8");
  QWC_EQ_I((int)p.mailbox[58], B_BISHOP, "B c8"); QWC_EQ_I((int)p.mailbox[59], B_QUEEN,  "B d8");
  QWC_EQ_I((int)p.mailbox[60], B_KING,   "B e8"); QWC_EQ_I((int)p.mailbox[61], B_BISHOP, "B f8");
  QWC_EQ_I((int)p.mailbox[62], B_KNIGHT, "B g8"); QWC_EQ_I((int)p.mailbox[63], B_ROOK,   "B h8");
  for (int s = 16; s < 48; s++)
    QWC_EQ_I((int)p.mailbox[s], NO_PIECE, "middle ranks empty");

  QWC_EQ_I((int)popcount(p.occ), 32, "32 pieces");
  QWC_EQ_I((int)popcount(p.byColor[WHITE]), 16, "16 white");
  QWC_EQ_I((int)popcount(p.byColor[BLACK]), 16, "16 black");
  QWC_EQ_U64(p.occ, p.byColor[WHITE] | p.byColor[BLACK], "occ = colors");
  QWC_EQ_I((int)popcount(p.byPiece[W_PAWN] | p.byPiece[B_PAWN]), 16, "16 pawns");
  QWC_EQ_I((int)popcount(p.byPiece[W_KNIGHT] | p.byPiece[B_KNIGHT]), 4, "4 knights");
  QWC_EQ_I((int)popcount(p.byPiece[W_BISHOP] | p.byPiece[B_BISHOP]), 4, "4 bishops");
  QWC_EQ_I((int)popcount(p.byPiece[W_ROOK] | p.byPiece[B_ROOK]), 4, "4 rooks");
  QWC_EQ_I((int)popcount(p.byPiece[W_QUEEN] | p.byPiece[B_QUEEN]), 2, "2 queens");
  QWC_EQ_I((int)popcount(p.byPiece[W_KING] | p.byPiece[B_KING]), 2, "2 kings");

  QWC_EQ_I((int)p.wKingSq, 4, "wKing e1");   /* e1 */
  QWC_EQ_I((int)p.bKingSq, 60, "bKing e8");  /* e8 */
  QWC_EQ_I((int)p.side, WHITE, "white to move");
  QWC_EQ_I((int)p.cr, 15, "all castling rights");
  QWC_EQ_I((int)p.ep_sq, NO_SQUARE, "no ep");
  QWC_EQ_I((int)p.halfmove, 0, "halfmove 0");
  QWC_EQ_I((int)p.fullmove, 1, "fullmove 1");

  QWC_TRUE(pos_validate(&p, NULL), "startpos validates");
  QWC_EQ_U64(p.key, expected_key(&p, -1), "startpos key");
  return qwc_end();
}

/* ---- explicitly constructed no-EP positions ---- */
static void build_kings(Position *pos, Square wk, Square bk, Color side) {
  pos_reset(pos);
  place(pos, wk, W_KING);
  place(pos, bk, B_KING);
  pos->side = side;
  pos->cr = 0;
  pos_rebuild(pos);
}

static int t_constructed(void) {
  qwc_begin("pos.constructed");
  Position k; build_kings(&k, 4, 60, WHITE);   /* e1/e8 */
  QWC_EQ_I((int)popcount(k.occ), 2, "kings-only: 2 pieces");
  QWC_EQ_I((int)k.wKingSq, 4, "kings-only wKing");
  QWC_EQ_I((int)k.bKingSq, 60, "kings-only bKing");
  QWC_EQ_I((int)k.ep_sq, NO_SQUARE, "kings-only no ep");
  QWC_TRUE(pos_validate(&k, NULL), "kings-only validates");
  QWC_EQ_U64(k.key, expected_key(&k, -1), "kings-only key");

  Position kb; build_kings(&kb, 5, 61, BLACK);  /* f1/f8, Black to move */
  QWC_EQ_I((int)kb.side, BLACK, "black to move");
  QWC_TRUE(pos_validate(&kb, NULL), "black-to-move validates");
  QWC_EQ_U64(kb.key, expected_key(&kb, -1), "black-to-move key");
  QWC_TRUE(kb.key != k.key, "side changes the key");
  return qwc_end();
}

/* ---- reset is a valid empty intermediate; instances are isolated ---- */
static int t_reset_isolation(void) {
  qwc_begin("pos.reset_isolation");
  Position e; pos_reset(&e);
  QWC_EQ_I((int)popcount(e.occ), 0, "reset: empty");
  QWC_EQ_I((int)e.wKingSq, NO_SQUARE, "reset: no white king");
  QWC_EQ_I((int)e.bKingSq, NO_SQUARE, "reset: no black king");
  QWC_EQ_I((int)e.side, WHITE, "reset: white");
  QWC_EQ_I((int)e.cr, 0, "reset: no rights");
  QWC_EQ_I((int)e.ep_sq, NO_SQUARE, "reset: no ep");
  QWC_TRUE(pos_validate(&e, NULL) == 0, "reset (no kings) rejected");
  QWC_EQ_U64(e.key, expected_key(&e, -1), "reset key == empty");

  /* Two separate instances must not share or affect each other's storage. */
  Position a, b; pos_set_startpos(&a); pos_set_startpos(&b);
  u64 akey = a.key;
  a.mailbox[4] = NO_PIECE; pos_rebuild(&a);  /* remove the white king from a */
  QWC_TRUE(pos_validate(&a, NULL) == 0, "a (king removed) rejected");
  QWC_TRUE(pos_validate(&b, NULL), "b unaffected by a");
  QWC_EQ_U64(b.key, akey, "b key unchanged");
  pos_reset(&a); pos_set_startpos(&a);
  QWC_TRUE(pos_validate(&a, NULL), "a restored");
  return qwc_end();
}

/* Build a legal en-passant setup for `side` to move: a double-pushed enemy pawn
 * on the ep file and (if with_captor) an adjacent friendly pawn, with the target
 * recorded. Kings sit clear of the action. (These are constructed directly; the
 * T003 setup paths do not produce EP -- FEN parsing lands in the next cp.) */
static Position ep_cfg(Square wk, Square bk, Color side, int f, int r, int with_captor) {
  Position p; pos_reset(&p);
  p.mailbox[wk] = W_KING;
  p.mailbox[bk] = B_KING;
  int pr = (side == WHITE) ? r - 1 : r + 1;   /* rank the two pawns share */
  Piece pushed = (side == WHITE) ? B_PAWN : W_PAWN;   /* the double-pushed pawn */
  p.mailbox[sq_of(f, pr)] = pushed;
  if (with_captor) {
    Piece cap = (side == WHITE) ? W_PAWN : B_PAWN;    /* the capturing pawn */
    p.mailbox[sq_of(f + 1, pr)] = cap;
  }
  p.side = side; p.cr = 0; p.ep_sq = sq_of(f, r);
  pos_rebuild(&p);
  return p;
}

static int t_canonical(void) {
  qwc_begin("pos.canonical_ep");
  Position w = ep_cfg(4, 60, WHITE, 3, 5, 1);   /* d6 target; black d5, white e5 */
  QWC_EQ_I(pos_canon_ep_file(&w), 3, "white legal ep -> file d");
  QWC_TRUE(pos_validate(&w, NULL), "white ep position validates");
  QWC_EQ_U64(w.key, expected_key(&w, 3), "white ep key includes ep-file d");
  QWC_TRUE((w.key ^ expected_key(&w, -1)) != 0, "ep-file key is non-zero");

  Position b = ep_cfg(4, 60, BLACK, 3, 2, 1);   /* d3 target; white d4, black e4 */
  QWC_EQ_I(pos_canon_ep_file(&b), 3, "black legal ep -> file d");
  QWC_TRUE(pos_validate(&b, NULL), "black ep position validates");
  QWC_EQ_U64(b.key, expected_key(&b, 3), "black ep key includes ep-file d");

  Position bad = ep_cfg(4, 60, WHITE, 3, 5, 0);  /* d6 target, no captor */
  QWC_EQ_I(pos_canon_ep_file(&bad), -1, "no captor -> no legal ep");
  QWC_TRUE(pos_validate(&bad, NULL), "uncapturable-but-valid ep is accepted");
  QWC_EQ_U64(bad.key, expected_key(&bad, -1), "uncapturable ep: no key contribution");

  /* A structurally invalid ep target (recorded on the wrong rank) is rejected. */
  Position malformed = bad;
  malformed.ep_sq = sq_of(3, 4);   /* d5: not on the white target rank (rank 6) */
  pos_rebuild(&malformed);
  QWC_TRUE(pos_validate(&malformed, NULL) == 0, "wrong-rank ep rejected");
  return qwc_end();
}

/* ---- rule-50 / fullmove counter bounds; key is independent of them ---- */
static int t_counters(void) {
  qwc_begin("pos.counters");
  Position p; pos_set_startpos(&p);
  u64 base = p.key;
  for (int hm = 0; hm <= 100; hm += 50) {
    p.halfmove = (HalfMoveClock)hm;
    pos_rebuild(&p);
    QWC_TRUE(pos_validate(&p, NULL), "valid halfmove accepted");
    QWC_EQ_U64(p.key, base, "key unchanged by halfmove");
  }
  for (int fm = 1; fm <= 999; fm += 500) {
    p.fullmove = (FullMoveNumber)fm;
    pos_rebuild(&p);
    QWC_TRUE(pos_validate(&p, NULL), "valid fullmove accepted");
    QWC_EQ_U64(p.key, base, "key unchanged by fullmove");
  }
  p.halfmove = (HalfMoveClock)200; pos_rebuild(&p);
  QWC_TRUE(pos_validate(&p, NULL), "draw threshold is not a structural bound");
  p.halfmove = 0;
  p.fullmove = 0; pos_rebuild(&p);
  QWC_TRUE(pos_validate(&p, NULL) == 0, "fullmove 0 rejected");
  return qwc_end();
}

/* A field-at-a-time corruption must be rejected, and the read-only validator
 * must not mutate the input (it never repairs a stale cache). */
static int t_corruption(void) {
  qwc_begin("pos.corruption");
  Position ok; pos_set_startpos(&ok);
  Position c;
  const char *why = NULL;

  memcpy(&c, &ok, sizeof c); c.mailbox[16] = (u8)159;
  QWC_TRUE(pos_validate(&c, &why) == 0, "invalid piece code rejected");
  QWC_TRUE(c.mailbox[16] == 159, "validator did not mutate mailbox");

  memcpy(&c, &ok, sizeof c); c.mailbox[4] = NO_PIECE; pos_rebuild(&c);
  QWC_TRUE(pos_validate(&c, &why) == 0, "missing white king rejected");
  QWC_TRUE(c.mailbox[4] == NO_PIECE, "validator did not restore king");

  memcpy(&c, &ok, sizeof c); c.mailbox[16] = W_KING; pos_rebuild(&c);
  QWC_TRUE(pos_validate(&c, &why) == 0, "extra white king rejected");
  QWC_TRUE(c.mailbox[16] == W_KING, "validator did not remove king");

  memcpy(&c, &ok, sizeof c); c.byColor[WHITE] |= square_bb(16);
  QWC_TRUE(pos_validate(&c, &why) == 0, "occupancy mismatch rejected");
  QWC_TRUE((c.byColor[WHITE] & square_bb(16)) != 0, "validator did not mutate byColor");

  memcpy(&c, &ok, sizeof c); c.byPiece[W_PAWN] |= square_bb(16);
  QWC_TRUE(pos_validate(&c, &why) == 0, "piece bitboard mismatch rejected");
  QWC_TRUE((c.byPiece[W_PAWN] & square_bb(16)) != 0, "validator did not mutate byPiece");

  memcpy(&c, &ok, sizeof c); c.wKingSq = 3;   /* d1, but the king is on e1 */
  QWC_TRUE(pos_validate(&c, &why) == 0, "stale wKingSq rejected");
  QWC_TRUE(c.wKingSq == 3, "validator did not mutate wKingSq");
  memcpy(&c, &ok, sizeof c); c.bKingSq = 59;  /* d8, but the king is on e8 */
  QWC_TRUE(pos_validate(&c, &why) == 0, "stale bKingSq rejected");
  QWC_TRUE(c.bKingSq == 59, "validator did not mutate bKingSq");

  memcpy(&c, &ok, sizeof c); c.cr = (u8)16;
  QWC_TRUE(pos_validate(&c, &why) == 0, "invalid rights rejected");
  QWC_TRUE((int)c.cr == 16, "validator did not mutate cr");

  memcpy(&c, &ok, sizeof c); c.side = (u8)2;
  QWC_TRUE(pos_validate(&c, &why) == 0, "invalid side rejected");
  QWC_TRUE((int)c.side == 2, "validator did not mutate side");

  memcpy(&c, &ok, sizeof c); c.fullmove = 0;
  QWC_TRUE(pos_validate(&c, &why) == 0, "fullmove 0 rejected");
  QWC_TRUE(c.fullmove == 0, "validator did not mutate fullmove");

  memcpy(&c, &ok, sizeof c); c.key ^= (u64)0x1234;
  QWC_TRUE(pos_validate(&c, &why) == 0, "changed key rejected");
  QWC_TRUE((c.key & (u64)0x1234) != 0, "validator did not mutate key");
  return qwc_end();
}

int main(void) {
  int fail = 0;
  fail |= t_startpos();
  fail |= t_constructed();
  fail |= t_reset_isolation();
  fail |= t_canonical();
  fail |= t_counters();
  fail |= t_corruption();
  return fail ? 1 : 0;
}
