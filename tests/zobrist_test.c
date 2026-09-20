/* QwenChess T002 test -- group 4: independent verification of Zobrist hashing.
 *
 * Part A (this section first): the production key tables must equal values
 * derived INDEPENDENTLY of the C code. The constants below come from
 * tools/zobrist_gen.py (a standalone SplitMix64 with the same documented seed
 * and draw order) and were cross-checked byte-identical against a build of
 * src/core/zobrist.c. Checking specific draw indices (beginning + end of each
 * family, both colors, every castling/ep entry) catches table-fill and
 * indexing mistakes -- not merely two calls to the same initializer.
 *
 * Part B (added next): XOR-delta updates vs an independent full recompute over
 * explicit synthetic before/after states. No Position/FEN/legality exists yet;
 * this validates the hashing foundations, not future make/unmake.
 *
 * Documented convention (src/core/zobrist.h):
 *   key = (XOR over occupied squares of zobrist_piece(piece, square))
 *         ^ zobrist_side(side)          (WHITE -> 0, BLACK -> key)
 *         ^ zobrist_castling(rights)    (rights 0..15; 0 = no rights is a key)
 *         ^ (ep_file >= 0 ? zobrist_ep_file(ep_file) : 0)
 * Rule-50 and the repetition history are deliberately NOT part of the key.
 * Castling-rights bit convention used by the fixtures: WK=1 WQ=2 BK=4 BQ=8.
 */
#include <string.h>
#include <time.h>
#include "core/types.h"
#include "core/zobrist.h"
#include "test_util.h"

/* Independent expected key values (see tools/zobrist_gen.py).
 * Piece rows 0,7,8,15 are NO_PIECE/unused slots: drawn to keep the PRNG stream
 * aligned but excluded from any position key. Both colors are covered (W 1..6,
 * B 9..14) at the a1 (first square) and h8 (last square) anchors. */
static const u64 P_A1[16]   = { 0x6E789E6AA1B965F4ull, 0x06C45D188009454Full, 0xF88BB8A8724C81ECull, 0x1B39896A51A8749Bull, 0x53CB9F0C747EA2EAull, 0x2C829ABE1F4532E1ull, 0xC584133AC916AB3Cull, 0x3EE5789041C98AC3ull, 0xF3B8488C368CB0A6ull, 0x657EECDD3CB13D09ull, 0xC2D326E0055BDEF6ull, 0x8621A03FE0BBDB7Bull, 0x8E1F7555983AA92Full, 0xB54E0F1600CC4D19ull, 0x84BB3F97971D80ABull, 0x7D29825C75521255ull };
static const u64 P_H8[16]   = { 0x8FF949D9B2334C46ull, 0x39D99BEF581FE0D3ull, 0x70B322ADC0939F80ull, 0xAF273E82D37541BCull, 0xF43D8225366812AAull, 0x7DC7392B1197006Full, 0x619C7313CB6308FCull, 0xF6DD0BFF853210C5ull, 0x49CB12F89F2428EEull, 0x1A388CC8169DDA78ull, 0x293FA87171EC42A0ull, 0xB055491D413A1636ull, 0xB350348997B6ECDBull, 0xF69029B901EFA3ADull, 0x2CDF2105AB2A3571ull, 0x6D6409C74776D986ull };
static const u64 SIDE_BLACK = 0x02EC155877FE5197ull;
static const u64 CASTLE[16] = { 0x9ACB73C63488B544ull, 0xEDE8F6A37FCC2CB7ull, 0xA1EDCBB1FEC6412Dull, 0x386D687E05228A93ull, 0x3F85C7FE3F785264ull, 0x6F639EDCC040B8E5ull, 0xDFCE21F83B7755FFull, 0xA601BD724DE89BBCull, 0x8AAC30D865AB3CB7ull, 0xBD7AE693AFC834AEull, 0x20192F04754C32B2ull, 0xC100953EA60B69D7ull, 0x973999A9D24AA499ull, 0xE7240B58F7437B66ull, 0xE668587F1D4A037Full, 0x06AE1C54DD761A39ull };
static const u64 EPFILE[8]  = { 0xCA07D06BEE139491ull, 0x6393C105DDE1C64Eull, 0xD5255D8C378B98E2ull, 0xD4552612C1F1D4F9ull, 0x30690C25ED4F7CEFull, 0xDC34E363ACA033E7ull, 0xFA7A7E12A7B013CFull, 0xFAF3411EC4419276ull };

/* ---- Part A: deterministic key generation ---- */
static int t_keys(void) {
  qwc_begin("zobrist.keys");
  for (int p = 0; p < 16; p++) {
    QWC_EQ_U64(zobrist_piece((Piece)p, 0),  P_A1[p], "piece[a1]");
    QWC_EQ_U64(zobrist_piece((Piece)p, 63), P_H8[p], "piece[h8]");
  }
  QWC_EQ_U64(zobrist_side(WHITE), 0,        "side white == 0");
  QWC_EQ_U64(zobrist_side(BLACK), SIDE_BLACK, "side black");
  for (int i = 0; i < 16; i++) QWC_EQ_U64(zobrist_castling((u8)i), CASTLE[i], "castling[i]");
  for (int f = 0; f < 8; f++)  QWC_EQ_U64(zobrist_ep_file(f),      EPFILE[f], "epfile[f]");
  /* Documented out-of-range guards (zobrist.h): must yield the identity 0. */
  QWC_EQ_U64(zobrist_piece((Piece)16, 0),    0, "piece p=16 out-of-range");
  QWC_EQ_U64(zobrist_piece((Piece)1, (Square)64), 0, "piece sq=64 out-of-range");
  QWC_EQ_U64(zobrist_ep_file(-1),            0, "epfile -1 none");
  QWC_EQ_U64(zobrist_ep_file(8),             0, "epfile 8 out-of-range");
  QWC_EQ_U64(zobrist_side((Color)2),         0, "side color=2 out-of-range");
  return qwc_end();
}

/* Idempotent + deterministic reinitialization: redrawing from the fixed seed
 * must reproduce every value (no public-contract change). */
static int t_reinit(void) {
  qwc_begin("zobrist.reinit");
  u64 a1 = zobrist_piece(W_PAWN, 0), h8 = zobrist_piece(B_KING, 63);
  u64 bl = zobrist_side(BLACK), c3 = zobrist_castling(3), e5 = zobrist_ep_file(5);
  zobrist_init();
  zobrist_init();
  QWC_EQ_U64(zobrist_piece(W_PAWN, 0),  a1, "reinit piece[a1]");
  QWC_EQ_U64(zobrist_piece(B_KING, 63), h8, "reinit piece[h8]");
  QWC_EQ_U64(zobrist_side(BLACK),       bl, "reinit side");
  QWC_EQ_U64(zobrist_castling(3),       c3, "reinit castling");
  QWC_EQ_U64(zobrist_ep_file(5),        e5, "reinit epfile");
  return qwc_end();
}

/* ---- Part B: XOR-delta updates vs an independent full recompute ----
 * Test-only snapshot; ep_file is the canonical ep target file, or -1 for none.
 * key_full() is the independent full recompute: it scans all 64 squares and
 * folds in the documented state keys, reading the (now-verified) production key
 * values but using NO delta/incremental logic. move_delta() is the separate
 * "incremental" path under test. check_move() cross-checks the two and also
 * verifies state restoration (apply/unapply) and grounds the reference against
 * the production full compute. */
typedef struct { u8 mailbox[64]; Color side; u8 cr; int ep_file; } TState;

/* A reversible board change. `pre`/`post` differ only on a promotion; `cap` is
 * the captured piece (NO_PIECE if none) at `cap_sq` (== to, except en-passant
 * where it is the captured pawn's ACTUAL square). An optional second leg
 * (from2 != NO_SQUARE) models castling's rook. The _b / _a fields hold the
 * before / after metadata so unapply() restores the exact prior state. */
typedef struct {
  Piece pre, post;   Square from, to;
  Piece cap;         Square cap_sq;
  Piece pre2, post2; Square from2, to2;
  Color side_b, side_a; u8 cr_b, cr_a; int ep_b, ep_a;
} TMove;

#define SQ(f, r) ((r) * 8 + (f))

static u64 epkey(int f) { return (f >= 0 && f < FILE_NB) ? zobrist_ep_file(f) : 0; }

static u64 key_full(const TState *s) {
  u64 h = 0;
  for (int sq = 0; sq < SQ_NB; sq++)
    if (s->mailbox[sq]) h ^= zobrist_piece((Piece)s->mailbox[sq], (Square)sq);
  h ^= zobrist_side(s->side) ^ zobrist_castling(s->cr) ^ epkey(s->ep_file);
  return h;
}

static void apply_move(TState *s, const TMove *m) {
  s->mailbox[m->from] = NO_PIECE;
  if (m->cap) s->mailbox[m->cap_sq] = NO_PIECE;
  s->mailbox[m->to] = m->post;
  if (m->from2 != NO_SQUARE) { s->mailbox[m->from2] = NO_PIECE; s->mailbox[m->to2] = m->post2; }
  s->side = m->side_a; s->cr = m->cr_a; s->ep_file = m->ep_a;
}
static void unapply_move(TState *s, const TMove *m) {
  s->mailbox[m->to] = NO_PIECE;
  s->mailbox[m->from] = m->pre;
  if (m->cap) s->mailbox[m->cap_sq] = m->cap;
  if (m->from2 != NO_SQUARE) { s->mailbox[m->to2] = NO_PIECE; s->mailbox[m->from2] = m->pre2; }
  s->side = m->side_b; s->cr = m->cr_b; s->ep_file = m->ep_b;
}
static int states_equal(const TState *a, const TState *b) {
  return a->side == b->side && a->cr == b->cr && a->ep_file == b->ep_file &&
         memcmp(a->mailbox, b->mailbox, 64) == 0;
}
/* The XOR-delta under test: exactly the keys that change, per the documented
 * convention (piece in/out, captured piece at its actual square, side, rights,
 * ep-file). En-passant is handled because cap_sq is the captured pawn's real
 * square, not `to`. */
static u64 move_delta(const TMove *m) {
  u64 d = zobrist_piece(m->pre, m->from) ^ zobrist_piece(m->post, m->to);
  if (m->cap) d ^= zobrist_piece(m->cap, m->cap_sq);
  if (m->from2 != NO_SQUARE) d ^= zobrist_piece(m->pre2, m->from2) ^ zobrist_piece(m->post2, m->to2);
  d ^= zobrist_side(m->side_b) ^ zobrist_side(m->side_a)
     ^ zobrist_castling(m->cr_b) ^ zobrist_castling(m->cr_a)
     ^ epkey(m->ep_b) ^ epkey(m->ep_a);
  return d;
}
static void check_move(const char *name, const TState *b, const TState *a, const TMove *m) {
  u64 d = move_delta(m), kb = key_full(b), ka = key_full(a);
  QWC_EQ_U64(kb ^ d, ka, name);                       /* incremental == full recompute */
  QWC_EQ_U64(ka ^ d, kb, name);                       /* XOR is its own inverse (reverse) */
  QWC_EQ_U64(ka, zobrist_compute(a->mailbox, a->side, a->cr, a->ep_file), name);  /* ground */
  QWC_EQ_U64(kb, zobrist_compute(b->mailbox, b->side, b->cr, b->ep_file), name);
  TState fwd = *b; apply_move(&fwd, m);
  QWC_TRUE(states_equal(&fwd, a), name);             /* applying reproduces the after-state */
  TState rst = *a;  unapply_move(&rst, m);
  QWC_TRUE(states_equal(&rst, b), name);             /* restored state */
  QWC_EQ_U64(key_full(&rst), kb, name);              /* restored key */
}

/* Standard initial position (16 pieces, both colors, all four rights). */
static TState base(void) {
  TState s; memset(&s, 0, sizeof s);
  const u8 wr[] = { W_ROOK, W_KNIGHT, W_BISHOP, W_QUEEN, W_KING, W_BISHOP, W_KNIGHT, W_ROOK };
  const u8 br[] = { B_ROOK, B_KNIGHT, B_BISHOP, B_QUEEN, B_KING, B_BISHOP, B_KNIGHT, B_ROOK };
  for (int f = 0; f < 8; f++) {
    s.mailbox[SQ(f, 0)] = wr[f];
    s.mailbox[SQ(f, 1)] = W_PAWN;
    s.mailbox[SQ(f, 6)] = B_PAWN;
    s.mailbox[SQ(f, 7)] = br[f];
  }
  s.side = WHITE; s.cr = 15; s.ep_file = -1;
  return s;
}
static TMove mv(Piece pre, Piece post, Square from, Square to, Piece cap, Square cap_sq,
                Color sb, Color sa, u8 crb, u8 cra, int epb, int epa) {
  TMove m;
  m.pre = pre; m.post = post; m.from = from; m.to = to; m.cap = cap; m.cap_sq = cap_sq;
  m.pre2 = NO_PIECE; m.post2 = NO_PIECE; m.from2 = NO_SQUARE; m.to2 = NO_SQUARE;
  m.side_b = sb; m.side_a = sa; m.cr_b = crb; m.cr_a = cra; m.ep_b = epb; m.ep_a = epa;
  return m;
}
static TMove mv2(TMove m, Piece pre2, Piece post2, Square from2, Square to2) {
  m.pre2 = pre2; m.post2 = post2; m.from2 = from2; m.to2 = to2;
  return m;
}
/* Bare kings-only board (7th/8th ranks empty) so promotion fixtures are legal. */
static TState kings(void) {
  TState s; memset(&s, 0, sizeof s);
  s.mailbox[SQ(4,0)] = W_KING;  /* e1 */
  s.mailbox[SQ(4,7)] = B_KING;  /* e8 */
  s.side = WHITE; s.cr = 0; s.ep_file = -1;
  return s;
}

static int t_moves(void) {
  qwc_begin("zobrist.moves");
  {   /* F1: quiet knight (white) Ng1-f3 */
    TState b = base();
    TState a = base(); a.mailbox[SQ(6,0)] = NO_PIECE; a.mailbox[SQ(5,2)] = W_KNIGHT; a.side = BLACK;
    TMove m = mv(W_KNIGHT, W_KNIGHT, SQ(6,0), SQ(5,2), NO_PIECE, NO_SQUARE, WHITE, BLACK, 15, 15, -1, -1);
    check_move("quiet knight W g1f3", &b, &a, &m);
  }
  {   /* F2: quiet knight (black) Nb8-c6 */
    TState b = base(); b.side = BLACK;
    TState a = base(); a.mailbox[SQ(1,7)] = NO_PIECE; a.mailbox[SQ(2,5)] = B_KNIGHT; a.side = WHITE;
    TMove m = mv(B_KNIGHT, B_KNIGHT, SQ(1,7), SQ(2,5), NO_PIECE, NO_SQUARE, BLACK, WHITE, 15, 15, -1, -1);
    check_move("quiet knight B b8c6", &b, &a, &m);
  }
  {   /* F3: ordinary capture + side change, W Bf1xf3 */
    TState b = base(); b.mailbox[SQ(5,2)] = B_PAWN;
    TState a = base(); a.mailbox[SQ(5,0)] = NO_PIECE; a.mailbox[SQ(5,2)] = W_BISHOP; a.side = BLACK;
    TMove m = mv(W_BISHOP, W_BISHOP, SQ(5,0), SQ(5,2), B_PAWN, SQ(5,2), WHITE, BLACK, 15, 15, -1, -1);
    check_move("capture W Bf1xf3", &b, &a, &m);
  }
  {   /* F4a: quiet promotion to QUEEN, a7-a8 */
    TState b = kings(); b.mailbox[SQ(0,6)] = W_PAWN;
    TState a = kings(); a.mailbox[SQ(0,6)] = NO_PIECE; a.mailbox[SQ(0,7)] = W_QUEEN; a.side = BLACK;
    TMove m = mv(W_PAWN, W_QUEEN, SQ(0,6), SQ(0,7), NO_PIECE, NO_SQUARE, WHITE, BLACK, 0, 0, -1, -1);
    check_move("promo Q a8", &b, &a, &m);
  }
  {   /* F4b: quiet promotion to ROOK, b7-b8 */
    TState b = kings(); b.mailbox[SQ(1,6)] = W_PAWN;
    TState a = kings(); a.mailbox[SQ(1,6)] = NO_PIECE; a.mailbox[SQ(1,7)] = W_ROOK; a.side = BLACK;
    TMove m = mv(W_PAWN, W_ROOK, SQ(1,6), SQ(1,7), NO_PIECE, NO_SQUARE, WHITE, BLACK, 0, 0, -1, -1);
    check_move("promo R b8", &b, &a, &m);
  }
  {   /* F4c: quiet promotion to BISHOP, c7-c8 */
    TState b = kings(); b.mailbox[SQ(2,6)] = W_PAWN;
    TState a = kings(); a.mailbox[SQ(2,6)] = NO_PIECE; a.mailbox[SQ(2,7)] = W_BISHOP; a.side = BLACK;
    TMove m = mv(W_PAWN, W_BISHOP, SQ(2,6), SQ(2,7), NO_PIECE, NO_SQUARE, WHITE, BLACK, 0, 0, -1, -1);
    check_move("promo B c8", &b, &a, &m);
  }
  {   /* F4d: quiet promotion to KNIGHT, d7-d8 */
    TState b = kings(); b.mailbox[SQ(3,6)] = W_PAWN;
    TState a = kings(); a.mailbox[SQ(3,6)] = NO_PIECE; a.mailbox[SQ(3,7)] = W_KNIGHT; a.side = BLACK;
    TMove m = mv(W_PAWN, W_KNIGHT, SQ(3,6), SQ(3,7), NO_PIECE, NO_SQUARE, WHITE, BLACK, 0, 0, -1, -1);
    check_move("promo N d8", &b, &a, &m);
  }
  {   /* F4e: promotion capture a7xb8=Q (b8 holds a black knight) */
    TState b = kings(); b.mailbox[SQ(0,6)] = W_PAWN; b.mailbox[SQ(1,7)] = B_KNIGHT;
    TState a = kings(); a.mailbox[SQ(0,6)] = NO_PIECE; a.mailbox[SQ(1,7)] = W_QUEEN; a.side = BLACK;
    TMove m = mv(W_PAWN, W_QUEEN, SQ(0,6), SQ(1,7), B_KNIGHT, SQ(1,7), WHITE, BLACK, 0, 0, -1, -1);
    check_move("promo capture a7xb8=Q", &b, &a, &m);
  }
  {   /* F5: en passant exd6; the captured pawn is removed from its ACTUAL
         square (d5), not from `to` (e6). */
    TState b = base(); b.mailbox[SQ(4,1)] = NO_PIECE; b.mailbox[SQ(4,4)] = W_PAWN;   /* e5 */
    b.mailbox[SQ(3,6)] = NO_PIECE; b.mailbox[SQ(3,4)] = B_PAWN;                        /* d5 */
    b.side = WHITE; b.ep_file = 3;                                                     /* ep file d */
    TState a = base(); a.mailbox[SQ(4,1)] = NO_PIECE; a.mailbox[SQ(4,4)] = NO_PIECE;
    a.mailbox[SQ(4,5)] = W_PAWN; a.mailbox[SQ(3,6)] = NO_PIECE; a.mailbox[SQ(3,4)] = NO_PIECE;
    a.side = BLACK; a.ep_file = -1;
    TMove m = mv(W_PAWN, W_PAWN, SQ(4,4), SQ(4,5), B_PAWN, SQ(3,4), WHITE, BLACK, 15, 15, 3, -1);
    check_move("en passant exd6", &b, &a, &m);
  }
  {   /* F6: castling O-O; king e1-g1 and rook h1-f1, both white rights lost. */
    TState b = base(); b.mailbox[SQ(5,0)] = NO_PIECE; b.mailbox[SQ(6,0)] = NO_PIECE;  /* clear f1,g1 */
    TState a = base(); a.mailbox[SQ(4,0)] = NO_PIECE; a.mailbox[SQ(6,0)] = W_KING;
    a.mailbox[SQ(5,0)] = W_ROOK; a.mailbox[SQ(7,0)] = NO_PIECE; a.side = BLACK; a.cr = 12;
    TMove m = mv2(mv(W_KING, W_KING, SQ(4,0), SQ(6,0), NO_PIECE, NO_SQUARE, WHITE, BLACK, 15, 12, -1, -1),
                  W_ROOK, W_ROOK, SQ(7,0), SQ(5,0));
    check_move("castling O-O", &b, &a, &m);
  }
  {   /* F7: rights lost when the king moves (not castling). */
    TState b = base(); b.mailbox[SQ(4,1)] = NO_PIECE;   /* clear e2 so Ke1-e2 is possible */
    TState a = base(); a.mailbox[SQ(4,0)] = NO_PIECE; a.mailbox[SQ(4,1)] = W_KING;
    a.side = BLACK; a.cr = 12;
    TMove m = mv(W_KING, W_KING, SQ(4,0), SQ(4,1), NO_PIECE, NO_SQUARE, WHITE, BLACK, 15, 12, -1, -1);
    check_move("king move loses both W rights", &b, &a, &m);
  }
  {   /* F8: rights lost when the queenside rook moves. */
    TState b = base(); b.mailbox[SQ(0,1)] = NO_PIECE;   /* clear a2 so Ra1-a2 is possible */
    TState a = base(); a.mailbox[SQ(0,0)] = NO_PIECE; a.mailbox[SQ(0,1)] = W_ROOK;
    a.side = BLACK; a.cr = 13;
    TMove m = mv(W_ROOK, W_ROOK, SQ(0,0), SQ(0,1), NO_PIECE, NO_SQUARE, WHITE, BLACK, 15, 13, -1, -1);
    check_move("rook move loses WQ", &b, &a, &m);
  }
  {   /* F8b: rights lost on a rook capture (a1 rook takes on a2). */
    TState b = base(); b.mailbox[SQ(0,1)] = B_PAWN;     /* a black pawn sits on a2 */
    TState a = base(); a.mailbox[SQ(0,0)] = NO_PIECE; a.mailbox[SQ(0,1)] = W_ROOK;
    a.side = BLACK; a.cr = 13;
    TMove m = mv(W_ROOK, W_ROOK, SQ(0,0), SQ(0,1), B_PAWN, SQ(0,1), WHITE, BLACK, 15, 13, -1, -1);
    check_move("rook capture loses WQ", &b, &a, &m);
  }
  {   /* F9: en-passant target created by a double push. */
    TState b = base();
    TState a = base(); a.mailbox[SQ(4,1)] = NO_PIECE; a.mailbox[SQ(4,3)] = W_PAWN;   /* e2 -> e4 */
    a.side = BLACK; a.ep_file = 4;
    TMove m = mv(W_PAWN, W_PAWN, SQ(4,1), SQ(4,3), NO_PIECE, NO_SQUARE, WHITE, BLACK, 15, 15, -1, 4);
    check_move("double push creates ep target", &b, &a, &m);
  }
  {   /* F10: en-passant target expires on the opponent's quiet reply. */
    TState b = base(); b.side = BLACK; b.ep_file = 4;
    TState a = base(); a.mailbox[SQ(1,7)] = NO_PIECE; a.mailbox[SQ(2,5)] = B_KNIGHT;
    a.side = WHITE; a.ep_file = -1;
    TMove m = mv(B_KNIGHT, B_KNIGHT, SQ(1,7), SQ(2,5), NO_PIECE, NO_SQUARE, BLACK, WHITE, 15, 15, 4, -1);
    check_move("quiet reply expires ep target", &b, &a, &m);
  }
  {   /* F11: a new double push replaces the existing ep target. */
    TState b = base(); b.ep_file = 4;
    TState a = base(); a.mailbox[SQ(2,1)] = NO_PIECE; a.mailbox[SQ(2,3)] = W_PAWN;   /* c2 -> c4 */
    a.side = BLACK; a.ep_file = 2;
    TMove m = mv(W_PAWN, W_PAWN, SQ(2,1), SQ(2,3), NO_PIECE, NO_SQUARE, WHITE, BLACK, 15, 15, 4, 2);
    check_move("double push replaces ep target", &b, &a, &m);
  }
  {   /* F12: combination -- a capture that also expires the ep target and flips side. */
    TState b = base(); b.mailbox[SQ(3,4)] = B_PAWN; b.side = WHITE; b.ep_file = 4;
    TState a = base(); a.mailbox[SQ(3,0)] = NO_PIECE; a.mailbox[SQ(3,4)] = W_QUEEN;
    a.side = BLACK; a.ep_file = -1;
    TMove m = mv(W_QUEEN, W_QUEEN, SQ(3,0), SQ(3,4), B_PAWN, SQ(3,4), WHITE, BLACK, 15, 15, 4, -1);
    check_move("capture + ep expire + side", &b, &a, &m);
  }
  return qwc_end();
}

int main(void) {
  zobrist_init();
  clock_t t0 = clock();
  int fail = 0;
  fail |= t_keys();
  fail |= t_reinit();
  fail |= t_moves();
  printf("zobrist total %.3fs -> %s\n",
         (double)(clock() - t0) / CLOCKS_PER_SEC, fail ? "FAIL" : "PASS");
  return fail ? 1 : 0;
}
