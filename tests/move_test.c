/* QwenChess T002 test -- group 2: the 32-bit Move encode/decode.
 *
 * Representation-only. Covers the from/to fields, the three move-kind flag bits
 * and their predicates, the promotion field (all underpromotions + queen), the
 * null/no-move sentinel, reserved-bit validity, and a few hand-derived packed
 * values. This is NOT a legality test: encoding validity is distinct from chess
 * legality and no board is involved. Round-trips alone could pass if encode and
 * decode were mutually wrong, so the fixture section pins exact bit patterns. */
#include "core/types.h"
#include "test_util.h"

/* ---- from/to fields + the null sentinel ---- */
static int t_move_fields(void) {
  qwc_begin("move.fields");
  /* Every from/to value 0..63 round-trips exactly through the 6-bit fields. */
  for (int f = 0; f < SQ_NB; f++)
    for (int t = 0; t < SQ_NB; t++) {
      Move m = make_move((Square)f, (Square)t, 0, 0);
      QWC_EQ_I((int)move_from(m), f, "from round-trip");
      QWC_EQ_I((int)move_to(m), t, "to round-trip");
    }
  /* null / no-move sentinel is exactly value 0, and only value 0 is null. */
  QWC_EQ_I((int)move_none(), 0, "move_none is 0");
  QWC_TRUE(is_null_move(move_none()), "null move detected");
  QWC_TRUE(is_null_move(0), "0 is null");
  QWC_TRUE(!is_null_move(make_move(0, 1, 0, 0)), "real move not null");
  /* 6-bit boundary: NO_SQUARE (64) cannot be represented; it masks down to 0. */
  QWC_EQ_I((int)move_from(make_move(NO_SQUARE, 3, 0, 0)), 0,
           "NO_SQUARE masks to 0 in a 6-bit field");
  return qwc_end();
}

/* ---- flags + promo field: all 3-bit values round-trip; predicates agree ---- */
static int t_move_flags_promo(void) {
  qwc_begin("move.flags_promo");
  for (int fl = 0; fl < 8; fl++)
    for (int pr = 0; pr < 8; pr++) {
      Move m = make_move(10, 20, (u32)fl, (u32)pr);
      QWC_EQ_I((int)move_flags(m), fl, "flags round-trip");
      QWC_EQ_I((int)move_promo(m), pr, "promo round-trip");
      QWC_TRUE(is_ep(m) == ((fl & MV_EP) != 0), "is_ep matches its bit");
      QWC_TRUE(is_promotion(m) == ((fl & MV_PROMO) != 0), "is_promotion matches its bit");
      QWC_TRUE(is_castle(m) == ((fl & MV_CASTLE) != 0), "is_castle matches its bit");
      QWC_TRUE(move_repr_is_valid(m), "any 0..18-bit encoding is repr-valid");
    }
  return qwc_end();
}

/* ---- special move kinds built via the public API ---- */
static int t_move_kinds(void) {
  qwc_begin("move.kinds");
  Move q = make_move(4, 8, 0, 0);
  QWC_TRUE(!is_ep(q) && !is_promotion(q) && !is_castle(q), "quiet: no special flag");

  Move ep = make_move(35, 36, MV_EP, 0);
  QWC_TRUE(is_ep(ep) && !is_promotion(ep) && !is_castle(ep), "ep: only MV_EP");
  QWC_EQ_I((int)move_from(ep), 35, "ep from");
  QWC_EQ_I((int)move_to(ep), 36, "ep to");

  /* promotion: every target piece type (all underpromotions + queen) */
  for (int p = KNIGHT; p <= QUEEN; p++) {
    Move pr = make_move(48, 56, MV_PROMO, (u32)p);
    QWC_TRUE(is_promotion(pr) && !is_ep(pr) && !is_castle(pr), "promo: only MV_PROMO");
    QWC_EQ_I((int)move_promo(pr), p, "promo target round-trips");
  }
  QWC_EQ_I((int)move_promo(make_move(48, 56, MV_PROMO, KNIGHT)), KNIGHT, "underpromote N");
  QWC_EQ_I((int)move_promo(make_move(48, 56, MV_PROMO, BISHOP)), BISHOP, "underpromote B");
  QWC_EQ_I((int)move_promo(make_move(48, 56, MV_PROMO, ROOK)), ROOK, "underpromote R");
  QWC_EQ_I((int)move_promo(make_move(48, 56, MV_PROMO, QUEEN)), QUEEN, "promote Q");

  /* castling: from = king square, to = the rook's origin square */
  Move c = make_move(4, 7, MV_CASTLE, 0);
  QWC_TRUE(is_castle(c) && !is_ep(c) && !is_promotion(c), "castle: only MV_CASTLE");
  QWC_EQ_I((int)move_from(c), 4, "castle king from");
  QWC_EQ_I((int)move_to(c), 7, "castle rook to");
  return qwc_end();
}

/* ---- hand-derived packed fixtures: encode AND decode must agree on exact bits ---- */
static int t_move_fixtures(void) {
  qwc_begin("move.fixtures");
  /* a1->a2 quiet : 0 | (8<<6) | 0      | 0      = 0x0000200 */
  QWC_EQ_U64((u64)make_move(0, 8, 0, 0), 0x0000200ull, "a1a2 packed");
  /* a7->a8=Q    : 48 | (56<<6) | (2<<12) | (5<<16) = 0x00052E30 */
  QWC_EQ_U64((u64)make_move(48, 56, MV_PROMO, QUEEN), 0x00052E30ull, "a7a8Q packed");
  /* castle O-O  : 4 | (7<<6) | (4<<12) | 0       = 0x000041C4 */
  QWC_EQ_U64((u64)make_move(4, 7, MV_CASTLE, 0), 0x000041C4ull, "castle O-O packed");
  /* ep d5xd6    : 35 | (36<<6) | (1<<12) | 0     = 0x00001923 */
  QWC_EQ_U64((u64)make_move(35, 36, MV_EP, 0), 0x00001923ull, "ep d5xd6 packed");

  /* decode the queen-promotion fixture back through every accessor */
  Move a7a8 = make_move(48, 56, MV_PROMO, QUEEN);
  QWC_EQ_I((int)move_from(a7a8), 48, "a7a8 from");
  QWC_EQ_I((int)move_to(a7a8), 56, "a7a8 to");
  QWC_EQ_I((int)move_flags(a7a8), (int)MV_PROMO, "a7a8 flags");
  QWC_EQ_I((int)move_promo(a7a8), QUEEN, "a7a8 promo");
  return qwc_end();
}

/* ---- reserved-bit validity: bits 19..31 must be clear; 0..18 are allowed ---- */
static int t_move_reserved(void) {
  qwc_begin("move.reserved");
  /* the maximum make_move output sets bits 0..18 only -> still repr-valid */
  QWC_TRUE(move_repr_is_valid(make_move(63, 63, 7, 7)), "all active bits set -> valid");
  /* each reserved bit 19..31, set alone, is invalid */
  for (int b = 19; b < 32; b++)
    QWC_TRUE(!move_repr_is_valid((Move)(1u << b)), "reserved bit set -> invalid");
  /* the top active bit (18) and the reserved flag slot (15) are outside 19..31 */
  QWC_TRUE(move_repr_is_valid((Move)(1u << 18)), "bit 18 (top promo) -> valid");
  QWC_TRUE(move_repr_is_valid((Move)(1u << 15)), "bit 15 (reserved flag slot) -> valid");
  return qwc_end();
}

int main(void) {
  int fail = 0;
  fail |= t_move_fields();
  fail |= t_move_flags_promo();
  fail |= t_move_kinds();
  fail |= t_move_fixtures();
  fail |= t_move_reserved();
  return fail ? 1 : 0;
}
