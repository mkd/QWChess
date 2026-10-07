/* QwenChess T004 checkpoint-2 test: en-passant capture make/unmake.
 *
 * Exercises the reversible en-passant capture that checkpoint 2 adds to
 * pos_make_move/pos_unmake_move: a 3-edit pseudo-legal application (captor
 * origin -> empty, captured pawn -> empty, empty target -> captor) with a full
 * restore on unmake. It does NOT perform legality (own-king safety) -- that is
 * movegen's job (T005) -- so both king-safe and king-unsafe captures are
 * applied and reversed; the unsafe ones are simply not pos_validate()d.
 *
 * The en-passant cases here are the mirror images of the eight double-push
 * fixtures already proven in fen_test.c (same boards, same legal/canon labels,
 * same pos_mirror reflection); a double push that creates a fully-legal capture
 * target, the other creates a malformed record that a capture rejects.
 *
 * Cases: the two exact before/after-FEN captures (white e5xd6, black e4xd3);
 * the eight fixtures, originals and rank-mirrored (Black mirrors increment the
 * fullmove); a double-push-then-capture sequence with a full LIFO restore;
 * counter overflow bounds; and a malformed-record suite verifying every reject
 * leaves the Position and an initialized StateInfo byte-for-byte unchanged.
 */
#include "position/position.h"
#include "position/state.h"
#include "position/fen.h"
#include "core/types.h"
#include "core/square.h"
#include "test_util.h"
#include <string.h>

/* ---- small helpers -------------------------------------------------------- */

static Square sq_of(int f, int r) { return square_of(f, r); }
static int    reflect_int(int sq) { int f = file_of((Square)sq), r = rank_of((Square)sq); return (int)square_of(f, 7 - r); }
/* Field-by-field equality over the entire Position (same contract as
 * makeunmake_test.c): a successful make/unmake pair must restore it exactly. */
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

/* Field-by-field equality over the undo record. */
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
 * produced exactly what a full rebuild would -- including the key. */
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

/* Rank-mirror a Position (same reflection as fen_test.c): flip each square's
 * rank, swap colors, reflect the recorded ep square, keep the rest, rebuild. */
static void pos_mirror(const Position *src, Position *dst) {
  memset(dst->mailbox, 0, SQ_NB);
  for (int s = 0; s < SQ_NB; s++) {
    int p = src->mailbox[s];
    if (!p) continue;
    int f = file_of((Square)s), r = rank_of((Square)s);
    dst->mailbox[square_of(f, 7 - r)] = (u8)(p ^ 8);
  }
  dst->side = (Color)(1 - src->side);
  dst->cr   = src->cr;
  if (src->ep_sq < SQ_NB) {
    int f = file_of(src->ep_sq), r = rank_of(src->ep_sq);
    dst->ep_sq = square_of(f, 7 - r);
  } else dst->ep_sq = NO_SQUARE;
  dst->halfmove = src->halfmove;
  dst->fullmove = src->fullmove;
  pos_rebuild(dst);
}

typedef struct { Square sq; Piece pc; } PC;

/* Build a fresh Position from an explicit piece list + the given metadata (the
 * ep argument lets us fabricate malformed en-passant records that fen_load would
 * refuse), then rebuild the derived caches. */
static Position mkpos(Color side, int cr, int hm, int fm, int ep, const PC *pcs, int n) {
  Position p; pos_reset(&p);
  p.side = side; p.cr = (u8)cr; p.halfmove = (HalfMoveClock)hm; p.fullmove = (FullMoveNumber)fm;
  p.ep_sq = (Square)ep;
  for (int i = 0; i < n; i++) p.mailbox[pcs[i].sq] = pcs[i].pc;
  pos_rebuild(&p);
  return p;
}
/* ---- the en-passant capture fixtures --------------------------------------
 *
 * These are the mirror images of the eight double-push fixtures in fen_test.c:
 * the same boards, with the en-passant target now recorded, and the capture
 * being the friendly pawn on the adjacent file taking the just-double-pushed
 * pawn. `from`/`to` are 0-based square indices. `ok` = the make succeeds (the
 * structure is a real capture); `safe` = the capture is king-safe (only those
 * are pos_validate()d -- the rest are pseudo-legal and simply reversed). The
 * safe ones also carry an exact expected resulting FEN. Fixture 5 has two
 * captors (c5 and e5): one king-safe, one king-unsafe -- the same split that
 * fen_test.c's canonical-file check already proves.
 */
typedef struct { int id; const char *fen; int from, to; int ok, safe; const char *after; } EpMk;

static const EpMk mk[] = {
  {  1, "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20",        36, 43, 1, 1, "k7/8/3P4/8/8/8/8/4K3 b - - 0 20" },
  {  2, "k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 20",     36, 43, 1, 0, NULL },
  {  3, "k7/8/8/r4pPK/8/8/8/8 w - f6 0 20",               38, 45, 1, 0, NULL },
  {  4, "k7/8/8/3pP3/4K3/8/8/8 w - d6 0 20",        36, 43, 1, 1, "k7/8/3P4/8/4K3/8/8/8 b - - 0 20" },
  {  5, "k3r3/8/8/2PpP3/8/8/8/4K3 w - d6 0 20",  34, 43, 1, 1, "k3r3/8/3P4/4P3/8/8/8/4K3 b - - 0 20" },
  {  5, "k3r3/8/8/2PpP3/8/8/8/4K3 w - d6 0 20",  36, 43, 1, 0, NULL },
  {  6, "k7/8/8/3p4/8/8/8/4K3 w - d6 0 20",        36, 43, 0, 0, NULL },
  {  7, "kb6/8/8/4Pp2/8/8/7K/8 w - f6 0 20",         36, 45, 1, 0, NULL },
  {  8, "4b2k/8/8/3pP3/K7/8/8/8 w - d6 0 20",  36, 43, 1, 0, NULL },
};
enum { MK_NB = (int)(sizeof mk / sizeof mk[0]) };

/* Attempt one capture on an already-loaded, consistent *pos and verify the full
 * reversible contract. `after` is the exact resulting FEN for king-safe cases
 * (NULL skips the FEN comparison but the board/scalars/rebuild are still checked).
 * A successful make is checked for the three square edits, the resulting board +
 * scalars, derived/key vs a rebuild, (if king-safe) validation + FEN, and a full
 * restore on undo. A rejected make must leave the Position and StateInfo intact. */
static int ep_body(Position *pos, int from, int to, int ok, int safe, const char *after) {
  Position before = *pos;
  StateInfo st; memset(&st, 0, sizeof st);
  Move mv = make_move((Square)from, (Square)to, MV_EP, 0u);
  const char *why = NULL;
  Color mside = (Color)before.side;

  if (!ok) {
    QWC_TRUE(pos_make_move(pos, &st, mv) == 0, "make rejected");
    StateInfo zero; memset(&zero, 0, sizeof zero);
    QWC_TRUE(pos_equal(pos, &before), "pos unchanged after reject");
    QWC_TRUE(state_equal(&st, &zero), "state unchanged after reject");
    return qwc_end();
  }

  QWC_TRUE(pos_make_move(pos, &st, mv) == 1, "make accepted");
  int victim = (mside == WHITE) ? (to - 8) : (to + 8);
  Piece mover = before.mailbox[from];
  Piece enemy = (Piece)((mside == WHITE) ? B_PAWN : W_PAWN);
  int full_after = (mside == BLACK) ? ((int)before.fullmove + 1) : ((int)before.fullmove);

  QWC_EQ_I(st.delta.edit_count, 3, "three square edits");
  QWC_EQ_I((int)st.delta.edits[0].sq, from, "edit[0].sq = captor origin");
  QWC_EQ_I((int)st.delta.edits[0].before, (int)mover, "edit[0].before = captor");
  QWC_EQ_I((int)st.delta.edits[0].after, (int)NO_PIECE, "edit[0].after = empty");
  QWC_EQ_I((int)st.delta.edits[1].sq, victim, "edit[1].sq = captured pawn");
  QWC_EQ_I((int)st.delta.edits[1].before, (int)enemy, "edit[1].before = victim");
  QWC_EQ_I((int)st.delta.edits[1].after, (int)NO_PIECE, "edit[1].after = empty");
  QWC_EQ_I((int)st.delta.edits[2].sq, to, "edit[2].sq = landing");
  QWC_EQ_I((int)st.delta.edits[2].before, (int)NO_PIECE, "edit[2].before = empty");
  QWC_EQ_I((int)st.delta.edits[2].after, (int)mover, "edit[2].after = captor");

  QWC_EQ_I((int)pos->mailbox[from], (int)NO_PIECE, "captor origin now empty");
  QWC_EQ_I((int)pos->mailbox[victim], (int)NO_PIECE, "captured pawn removed");
  QWC_EQ_I((int)pos->mailbox[to], (int)mover, "captor landed on target");

  QWC_EQ_I((int)pos->side, (int)(1 - mside), "side flipped");
  QWC_EQ_I((int)pos->ep_sq, (int)NO_SQUARE, "recorded ep cleared");
  QWC_EQ_I((int)pos->halfmove, 0, "halfmove reset (pawn capture)");
  QWC_EQ_I((int)pos->fullmove, full_after, "fullmove after");

  QWC_TRUE(derived_match_rebuild(pos, &why), "derived/key == rebuild");

  if (safe) {
    QWC_TRUE(pos_validate(pos, &why), "king-safe result validates");
    if (after) {
      char buf[FEN_BUF_SIZE];
      QWC_TRUE(fen_emit(pos, buf, (int)sizeof buf) == 1, "fen emit");
      printf("      evidence: %s\n", buf);
      QWC_TRUE(strcmp(buf, after) == 0, "exact resulting FEN");
    }
  }

  pos_unmake_move(pos, &st);
  QWC_TRUE(pos_equal(pos, &before), "undo restores exactly");
  return qwc_end();
}
/* ---- the two exact before/after-FEN captures ------------------------------ */
static int t_main(void) {
  int fail = 0;
  qwc_begin("ep_main.white_e5d6");
  Position pos; const char *why = NULL;
  QWC_TRUE(fen_load(&pos, "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20", &why) == 1, "load before");
  char fb[FEN_BUF_SIZE];
  QWC_TRUE(fen_emit(&pos, fb, (int)sizeof fb) == 1 && strcmp(fb, "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20") == 0, "before FEN round-trips");
  fail |= ep_body(&pos, 36, 43, 1, 1, "k7/8/3P4/8/8/8/8/4K3 b - - 0 20");

  qwc_begin("ep_main.black_e4d3");
  QWC_TRUE(fen_load(&pos, "4k3/8/8/8/3Pp3/8/8/K7 b - d3 0 20", &why) == 1, "load before");
  QWC_TRUE(fen_emit(&pos, fb, (int)sizeof fb) == 1 && strcmp(fb, "4k3/8/8/8/3Pp3/8/8/K7 b - d3 0 20") == 0, "before FEN round-trips");
  fail |= ep_body(&pos, 28, 19, 1, 1, "4k3/8/8/8/8/3p4/8/K7 w - - 0 21");
  return fail;
}

/* ---- the eight fixtures, originals (white to move) ------------------------ */
static int t_fixtures(void) {
  int fail = 0;
  for (int i = 0; i < MK_NB; i++) {
    char tag[64]; snprintf(tag, sizeof tag, "ep_fix.%d_%d", mk[i].id, mk[i].from);
    qwc_begin(tag);
    Position pos; const char *why = NULL;
    QWC_TRUE(fen_load(&pos, mk[i].fen, &why) == 1, "fixture loads");
    fail |= ep_body(&pos, mk[i].from, mk[i].to, mk[i].ok, mk[i].safe, mk[i].after);
  }
  return fail;
}

/* ---- the eight fixtures, rank-mirrored (black to move) -------------------- */
static int t_mirrors(void) {
  int fail = 0;
  for (int i = 0; i < MK_NB; i++) {
    char tag[64]; snprintf(tag, sizeof tag, "ep_mirror.%d_%d", mk[i].id, mk[i].from);
    qwc_begin(tag);
    Position w, pos; const char *why = NULL;
    QWC_TRUE(fen_load(&w, mk[i].fen, &why) == 1, "source loads");
    pos_mirror(&w, &pos);
    QWC_EQ_I((int)pos.side, (int)BLACK, "mirror is black to move");
    QWC_TRUE(pos_validate(&pos, &why), "mirror (pre-capture) validates");
    int mfrom = reflect_int(mk[i].from), mto = reflect_int(mk[i].to);
    fail |= ep_body(&pos, mfrom, mto, mk[i].ok, mk[i].safe, NULL);
  }
  return fail;
}
/* ---- a double push creating a legal EP target, then the capture ----------- */
static int t_sequence(void) {
  qwc_begin("ep_seq.push_then_ep");
  Position pos; const char *why = NULL;
  QWC_TRUE(fen_load(&pos, "7k/8/8/8/2p5/8/1P6/7K w - - 0 1", &why) == 1, "load");
  Position before = pos;
  StateInfo st[2]; memset(st, 0, sizeof st);

  /* 1.b2b4 (white double push) records the ep target b3 */
  QWC_TRUE(pos_make_move(&pos, &st[0], make_move(sq_of(1, 1), sq_of(1, 3), 0u, 0u)) == 1, "1.b2b4 accepted");
  QWC_EQ_I((int)pos.ep_sq, (int)sq_of(1, 2), "ep target b3 recorded");
  QWC_EQ_I((int)pos.side, (int)BLACK, "black to move after push");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "derived/key after push == rebuild");
  QWC_TRUE(pos_validate(&pos, &why), "post-push validates");
  Position after_push = pos;   /* snapshot: the position right after 1.b2b4 */

  /* 1...c4xb3 (black en passant) */
  QWC_TRUE(pos_make_move(&pos, &st[1], make_move(sq_of(2, 3), sq_of(1, 2), MV_EP, 0u)) == 1, "1...c4xb3 ep accepted");
  QWC_EQ_I((int)pos.ep_sq, (int)NO_SQUARE, "recorded ep cleared");
  QWC_EQ_I((int)pos.side, (int)WHITE, "white to move after ep");
  QWC_EQ_I((int)pos.fullmove, 2, "fullmove 2 after black move");
  QWC_EQ_I((int)pos.mailbox[sq_of(2, 3)], (int)NO_PIECE, "c4 now empty");
  QWC_EQ_I((int)pos.mailbox[sq_of(1, 3)], (int)NO_PIECE, "b4 (captured) removed");
  QWC_EQ_I((int)pos.mailbox[sq_of(1, 2)], (int)B_PAWN, "captor landed on b3");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "derived/key after ep == rebuild");
  QWC_TRUE(pos_validate(&pos, &why), "final position validates");
  char fb[FEN_BUF_SIZE];
  QWC_TRUE(fen_emit(&pos, fb, (int)sizeof fb) == 1 && strcmp(fb, "7k/8/8/8/8/1p6/8/7K w - - 0 2") == 0, "exact final FEN");

  /* undo both (LIFO) -> full restore. Undoing the ep restores the post-push
   * position (after 1.b2b4); undoing the push then restores the loaded one. */
  pos_unmake_move(&pos, &st[1]);
  QWC_TRUE(pos_equal(&pos, &after_push), "undo ep restores the post-push position");
  pos_unmake_move(&pos, &st[0]);
  QWC_TRUE(pos_equal(&pos, &before), "undo push restores the loaded position");
  return qwc_end();
}

/* ---- counter overflow bounds ------------------------------------------------ */
static int count_case(const char *tag, const char *fen, int fm, int from, int to, int ok, int fm_after) {
  qwc_begin(tag);
  Position pos; const char *why = NULL;
  QWC_TRUE(fen_load(&pos, fen, &why) == 1, "load");
  pos.fullmove = (FullMoveNumber)fm;
  Position before = pos;
  StateInfo st; memset(&st, 0, sizeof st);
  Move mv = make_move((Square)from, (Square)to, MV_EP, 0u);
  if (ok) {
    QWC_TRUE(pos_make_move(&pos, &st, mv) == 1, "ep accepted");
    QWC_EQ_I((int)pos.fullmove, fm_after, "fullmove after");
    QWC_TRUE(derived_match_rebuild(&pos, &why), "derived/key == rebuild");
    pos_unmake_move(&pos, &st);
    QWC_TRUE(pos_equal(&pos, &before), "undo restores fullmove");
  } else {
    QWC_TRUE(pos_make_move(&pos, &st, mv) == 0, "ep rejected (fullmove overflow)");
    StateInfo zero; memset(&zero, 0, sizeof zero);
    QWC_TRUE(pos_equal(&pos, &before), "pos unchanged");
    QWC_TRUE(state_equal(&st, &zero), "state unchanged");
    QWC_EQ_I((int)pos.fullmove, fm, "fullmove unchanged");
  }
  return qwc_end();
}

static int t_counters(void) {
  int fail = 0;
  const char *bep = "7k/8/8/8/1Pp5/8/8/7K b - b3 0 1";  /* black c4(26)xb3(17) */
  const char *wep = "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 1";  /* white e5(36)xd6(43) */
  fail |= count_case("ep_count.black_65534", bep, 65534, 26, 17, 1, 65535);
  fail |= count_case("ep_count.black_65535", bep, 65535, 26, 17, 0, 65535);
  fail |= count_case("ep_count.white_65535", wep, 65535, 36, 43, 1, 65535);
  return fail;
}
/* Assert a make is rejected and leaves BOTH the Position and an initialized
 * StateInfo byte-for-byte unchanged (the contract for every rejection). */
static void assert_reject(Position *pos, int from, int to, u32 flags, u32 promo) {
  Position before = *pos;
  StateInfo st; memset(&st, 0, sizeof st);
  StateInfo st0 = st;
  Move mv = make_move((Square)from, (Square)to, flags, promo);
  QWC_TRUE(pos_make_move(pos, &st, mv) == 0, "make rejected");
  QWC_TRUE(pos_equal(pos, &before), "pos unchanged");
  QWC_TRUE(state_equal(&st, &st0), "state unchanged");
}

static int reject_fen_case(const char *tag, const char *fen, int from, int to, u32 flags, u32 promo) {
  qwc_begin(tag);
  Position pos; const char *why = NULL;
  QWC_TRUE(fen_load(&pos, fen, &why) == 1, "fixture loads");
  assert_reject(&pos, from, to, flags, promo);
  return qwc_end();
}

static int reject_mkpos_case(const char *tag, Color side, int cr, int hm, int fm, int ep,
                             const PC *pcs, int n, int from, int to, u32 flags, u32 promo) {
  qwc_begin(tag);
  Position pos = mkpos(side, cr, hm, fm, ep, pcs, n);
  assert_reject(&pos, from, to, flags, promo);
  return qwc_end();
}

/* ---- malformed / invalid captures -----------------------------------------
 *
 * Each case breaks exactly one precondition of the EP application and must be
 * rejected, leaving the Position and an initialized StateInfo untouched. The
 * FEN-loaded cases exercise the target / geometry / source-ownership / flag
 * checks; the directly-constructed cases exercise the malformed double-push
 * record (pos_ep_is_valid_meta) that fen_load would itself refuse.
 */
static int t_reject(void) {
  int fail = 0;
  /* absent recorded target */
  fail |= reject_fen_case("ep_reject.absent_ep",
    "k7/8/8/3pP3/8/8/8/4K3 w - - 0 20", 36, 43, MV_EP, 0);
  /* wrong target (to the captured pawn's square, not the ep square) */
  fail |= reject_fen_case("ep_reject.wrong_target",
    "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20", 36, 35, MV_EP, 0);
  /* geometry: captor on the capture rank (not one forward) */
  fail |= reject_fen_case("ep_reject.bad_rank",
    "k7/8/2P5/3p4/8/8/8/4K3 w - d6 0 1", 42, 43, MV_EP, 0);
  /* geometry: captor two files away (not a diagonal) */
  fail |= reject_fen_case("ep_reject.bad_file",
    "k7/8/8/1P1p4/8/8/8/4K3 w - d6 0 1", 33, 43, MV_EP, 0);
  /* wrong source piece (a knight, not the friendly pawn) */
  fail |= reject_fen_case("ep_reject.source_knight",
    "k7/8/8/3pN3/8/8/8/4K3 w - d6 0 1", 36, 43, MV_EP, 0);
  /* wrong source piece (an enemy pawn) */
  fail |= reject_fen_case("ep_reject.source_enemy",
    "k7/8/8/3pp3/8/8/8/4K3 w - d6 0 1", 36, 43, MV_EP, 0);
  /* malformed record: own piece on the captured (just-pushed) square */
  static const PC own5[]  = { {36, W_PAWN}, {4, W_KING}, {35, W_PAWN}, {0, B_KING} };
  fail |= reject_mkpos_case("ep_reject.victim_own", WHITE, 0, 0, 1, 43, own5, 4, 36, 43, MV_EP, 0);
  /* malformed record: empty captured square */
  static const PC emp5[]   = { {36, W_PAWN}, {4, W_KING}, {0, B_KING} };
  fail |= reject_mkpos_case("ep_reject.victim_empty", WHITE, 0, 0, 1, 43, emp5, 3, 36, 43, MV_EP, 0);
  /* malformed record: an enemy non-pawn on the captured square */
  static const PC kn5[]    = { {36, W_PAWN}, {4, W_KING}, {35, B_KNIGHT}, {0, B_KING} };
  fail |= reject_mkpos_case("ep_reject.victim_nonpawn", WHITE, 0, 0, 1, 43, kn5, 4, 36, 43, MV_EP, 0);
  /* malformed record: the ep target itself is occupied */
  static const PC occ6[]   = { {36, W_PAWN}, {4, W_KING}, {0, B_KING}, {35, B_PAWN}, {43, B_PAWN} };
  fail |= reject_mkpos_case("ep_reject.target_occupied", WHITE, 0, 0, 1, 43, occ6, 5, 36, 43, MV_EP, 0);
  /* malformed record: the double-push origin is occupied */
  static const PC occ7[]   = { {36, W_PAWN}, {4, W_KING}, {0, B_KING}, {35, B_PAWN}, {51, B_PAWN} };
  fail |= reject_mkpos_case("ep_reject.origin_occupied", WHITE, 0, 0, 1, 43, occ7, 5, 36, 43, MV_EP, 0);
  /* malformed record: halfmove clock nonzero */
  static const PC hm5[]    = { {36, W_PAWN}, {4, W_KING}, {0, B_KING}, {35, B_PAWN} };
  fail |= reject_mkpos_case("ep_reject.halfmove_nonzero", WHITE, 0, 5, 1, 43, hm5, 4, 36, 43, MV_EP, 0);
  /* invalid flags: EP | promotion (mixed) */
  fail |= reject_fen_case("ep_reject.flags_mixed",
    "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20", 36, 43, MV_EP | MV_PROMO, QUEEN);
  /* invalid payload: a promotion type on an EP move */
  fail |= reject_fen_case("ep_reject.promo_payload",
    "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20", 36, 43, MV_EP, QUEEN);
  return fail;
}

int main(void) {
  int fail = 0;
  fail |= t_main();
  fail |= t_fixtures();
  fail |= t_mirrors();
  fail |= t_sequence();
  fail |= t_counters();
  fail |= t_reject();
  return fail ? 1 : 0;
}
