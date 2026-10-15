/* QwenChess T005 checkpoint-1 test: non-castling pseudo-legal move generation.
 *
 * Verifies movegen_pseudo_noncastling() (src/movegen/movegen.c) against the
 * independently-derived candidate sets (python-chess 1.11.2, checked at prompt
 * time). "Pseudo-legal" means correct piece geometry, occupancy and special-move
 * metadata; own-king safety and resolving check are deliberately NOT filtered at
 * this layer, so the fixture expected sets keep king moves to attacked squares
 * and keep a pinned captor's en-passant. Castling is excluded by the API name.
 *
 * Groups: the 14 fixtures and their rank-reflected/color-swapped mirrors
 * (complete encoded Move set, order-independent, no duplicates) + the flag /
 * payload semantics checked separately; the exact start position (20); the
 * deliberate king-safety boundary cases; a blocked-first-pawn-step case;
 * capacity/overflow with canaries; determinism and a live history prefix; the
 * counter-independence case; and a per-candidate apply/unmake round-trip.
 *
 * The coordinate strings are test-only notation; a small decoder turns them into
 * Moves (a promotion suffix sets MoveFlag::MV_PROMO; the per-fixture EP list sets MoveFlag::MV_EP).
 * This is NOT production UCI parsing.
 */
#include "movegen/movegen.hpp"
#include "position/position.hpp"
#include "position/state.hpp"
#include "position/fen.hpp"
#include "core/types.hpp"
#include "core/square.hpp"
#include "core/bitboard.hpp"
#include "test_util.hpp"

#include <cstring>
#include <cstdio>
#include <cstddef>

/* Test output buffer, larger than any fixture's move count. */
enum { MG_CAP = 256 };

/* ---- small test-only helpers --------------------------------------------- */

/* Decode a coordinate string ("a1a2" or "a7a8q") into a Move. A promotion suffix
 * (n/b/r/q) sets MoveFlag::MV_PROMO with the matching payload. Returns 0 if malformed. */
static int coord_to_move(const char *s, Move *out) {
  size_t len = std::strlen(s);
  if (len < 4 || len > 5) return 0;
  int ff = s[0] - 'a', fr = s[1] - '1', tf = s[2] - 'a', tr = s[3] - '1';
  if (ff < 0 || ff > 7 || fr < 0 || fr > 7 || tf < 0 || tf > 7 || tr < 0 || tr > 7)
    return 0;
  u32 flags = 0, promo = 0;
  if (len == 5) {
    switch (s[4]) {
      case 'n': promo = to_under(PieceType::KNIGHT); break;
      case 'b': promo = to_under(PieceType::BISHOP); break;
      case 'r': promo = to_under(PieceType::ROOK);   break;
      case 'q': promo = to_under(PieceType::QUEEN);  break;
      default:  return 0;
    }
    flags = (u32)MoveFlag::MV_PROMO;
  }
  *out = make_move((Square)square_of(ff, fr), (Square)square_of(tf, tr), flags, promo);
  return 1;
}

/* Reflect a square: same file, rank index 7-r (1-indexed rank 9-r). */
static Square reflect_sq(Square s) { return square_of(file_of(s), 7 - rank_of(s)); }

/* Swap the color bits of a castling-right value (K<->k, Q<->q). */
static u8 mirror_cr(u8 cr) {
  u8 r = 0;
  if (cr & CR_WK) r |= CR_BK;
  if (cr & CR_WQ) r |= CR_BQ;
  if (cr & CR_BK) r |= CR_WK;
  if (cr & CR_BQ) r |= CR_WQ;
  return r;
}

/* Rank-reflect + color-swap a Position (the fixture mirror). The ep square is
 * reflected, the castling rights' color bits swapped, and the history abandoned
 * (the mirror is a fresh setup for this test). */
static void pos_mirror(const Position *src, Position *dst) {
  std::memset(dst->mailbox, 0, SQ_NB);
  for (int s = 0; s < SQ_NB; s++) {
    int p = to_under(src->mailbox[s]);
    if (!p) continue;
    int f = file_of((Square)s), r = rank_of((Square)s);
    dst->mailbox[square_of(f, 7 - r)] = (Piece)(p ^ 8);
  }
  dst->side    = (Color)(1 - to_under(src->side));
  dst->cr      = mirror_cr(src->cr);
  dst->ep_sq   = (src->ep_sq < SQ_NB) ? reflect_sq(src->ep_sq) : (Square)NO_SQUARE;
  dst->halfmove = src->halfmove;
  dst->fullmove = src->fullmove;
  dst->history  = nullptr;
  pos_rebuild(dst);
}

/* Field-by-field equality over the entire Position (logical + derived caches).
 * A successful make/unmake pair, or a generation that must not mutate state,
 * restores every one of these. */
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

/* Do the derived caches + key match a from-scratch rebuild of the same board?
 * pos_rebuild recomputes byPiece/byColor/occ/kings/key (leaving the recorded ep,
 * side, cr and counters untouched), so this confirms an incremental make produced
 * exactly what a full rebuild would -- including the key. */
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

/* Split a space-separated buffer into NUL-terminated tokens (in place). */
static int split_spaces(char *s, char **toks, int maxtok) {
  int n = 0; char *p = s;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    if (n < maxtok) toks[n++] = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = '\0';
  }
  return n;
}

/* Copy a NUL-terminated string into `dst`, clamped to `cap` (never reads past
 * the source's terminator and never writes the buffer full without a terminator). */
static void strcopy(char *dst, size_t cap, const char *src) {
  size_t n = std::strlen(src);
  if (n >= cap) n = cap - 1;
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

/* Reflect every coordinate in a space-separated moveset string (each square's
 * rank becomes 9-rank; files and any promotion suffix are preserved). */
static void reflect_moveset(const char *src, char dst[512]) {
  char buf[512]; strcopy(buf, sizeof buf, src);
  char *toks[128]; int nt = split_spaces(buf, toks, 128);
  size_t di = 0;
  for (int i = 0; i < nt; i++) {
    if (i) dst[di++] = ' ';
    for (int k = 0; toks[i][k] != '\0'; k++) {
      char ch = toks[i][k];
      if (k == 1 || k == 3) { int d = ch - '0'; ch = (char)('0' + (9 - d)); }
      dst[di++] = ch;
    }
  }
  dst[di] = '\0';
}

/* In-place ascending sort of an array of Moves (the compact encoding is total). */
static void sort_moves(Move *a, size_t n) {
  for (size_t i = 1; i < n; i++) {
    Move key = a[i]; size_t j = i;
    while (j > 0 && a[j - 1] > key) { a[j] = a[j - 1]; j--; }
    a[j] = key;
  }
}

/* ---- the independent candidate sets (python-chess 1.11.2, prompt time) -----
 *
 * `moves` is the complete pseudo-legal NON-castling set for the position (the
 * same set the mirror must reproduce, reflected). `ep_moves` lists the subset of
 * those that carry MoveFlag::MV_EP (zero for the non-EP fixtures). All are white-to-move
 * except where noted; the mirror is black-to-move.
 */
typedef struct {
  const char *name;
  const char *fen;
  const char *moves;    /* complete expected set (space-separated coords) */
  const char *ep_moves; /* the subset carrying MoveFlag::MV_EP ("" = none)          */
} Fixture;

static const Fixture fixtures[] = {
  { "rook",
    "7k/8/8/8/8/3p4/3R4/K7 w - - 0 1",
    "a1a2 a1b1 a1b2 d2a2 d2b2 d2c2 d2d1 d2d3 d2e2 d2f2 d2g2 d2h2", "" },
  { "knight",
    "7k/8/8/8/8/8/8/KN6 w - - 0 1",
    "a1a2 a1b2 b1a3 b1c3 b1d2", "" },
  { "bishop",
    "7k/8/6p1/8/4B3/3P4/8/K7 w - - 0 1",
    "a1a2 a1b1 a1b2 d3d4 e4a8 e4b7 e4c6 e4d5 e4f3 e4f5 e4g2 e4g6 e4h1", "" },
  { "queen",
    "7k/8/4p1p1/8/4Q3/3P4/8/K7 w - - 0 1",
    "a1a2 a1b1 a1b2 d3d4 e4a4 e4a8 e4b4 e4b7 e4c4 e4c6 e4d4 e4d5 e4e1 "
    "e4e2 e4e3 e4e5 e4e6 e4f3 e4f4 e4f5 e4g2 e4g4 e4g6 e4h1 e4h4", "" },
  { "king_in_check",
    "7k/8/8/8/8/8/4r3/4K3 w - - 0 1",
    "e1d1 e1d2 e1e2 e1f1 e1f2", "" },
  { "promotion",
    "1r5k/P7/8/8/8/8/8/7K w - - 0 1",
    "a7a8b a7a8n a7a8q a7a8r a7b8b a7b8n a7b8q a7b8r h1g1 h1g2 h1h2", "" },
  { "blocked_double",
    "7k/8/8/8/4n3/8/3PP3/7K w - - 0 1",
    "d2d3 d2d4 e2e3 h1g1 h1g2 h1h2", "" },
  { "pawn_captures",
    "7k/8/8/2p1p3/3P4/8/8/K7 w - - 0 1",
    "a1a2 a1b1 a1b2 d4c5 d4d5 d4e5", "" },
  { "pawn_a_edge",
    "7k/8/8/8/P6p/8/8/4K3 w - - 0 1",
    "a4a5 e1d1 e1d2 e1e2 e1f1 e1f2", "" },
  { "pawn_h_edge",
    "k7/p7/8/7P/8/8/8/4K3 w - - 0 1",
    "e1d1 e1d2 e1e2 e1f1 e1f2 h5h6", "" },
  { "pinned_ep",
    "k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 20",
    "e1d1 e1d2 e1e2 e1f1 e1f2 e5d6 e5e6", "e5d6" },
  { "mixed_ep",
    "k3r3/8/8/2PpP3/8/8/8/4K3 w - d6 0 20",
    "c5c6 c5d6 e1d1 e1d2 e1e2 e1f1 e1f2 e5d6 e5e6", "c5d6 e5d6" },
  { "no_captor",
    "k7/8/8/3p4/8/8/8/4K3 w - d6 0 20",
    "e1d1 e1d2 e1e2 e1f1 e1f2", "" },
  { "castle_ready",
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1d1 e1d2 e1e2 "
    "e1f1 e1f2 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8", "" },
};
enum { NB_FIXTURES = (int)(sizeof fixtures / sizeof fixtures[0]) };

/* (qwc_begin already called) Compare the generated `got`/`ngot` against the
 * expected coord set. Verifies: the complete encoded Move set (order-independent
 * after sorting), no duplicates, and -- separately from the coordinates -- the
 * flag/payload semantics (MoveFlag::MV_EP count, MoveFlag::MV_PROMO count + range, no castling, no
 * mixed flags, no reserved bits, from != to, no null). Returns qwc_end(). */
static int cmp_set(const Move *got, size_t ngot, const char *moves, const char *ep_list) {
  Move exp[128]; size_t nexp = 0;
  char mbuf[512]; strcopy(mbuf, sizeof mbuf, moves);
  char *mtoks[128]; int mt = split_spaces(mbuf, mtoks, 128);
  char ebuf[256]; strcopy(ebuf, sizeof ebuf, ep_list);
  char *etoks[16];  int et = split_spaces(ebuf, etoks, 16);

  for (int i = 0; i < mt && nexp < 128; i++) {
    Move m = 0;
    QWC_TRUE(coord_to_move(mtoks[i], &m), "expected coord decodes");
    int is_ep = 0;
    for (int j = 0; j < et; j++) if (std::strcmp(etoks[j], mtoks[i]) == 0) { is_ep = 1; break; }
    if (is_ep) m = make_move(move_from(m), move_to(m), (u32)MoveFlag::MV_EP, 0u);
    exp[nexp++] = m;
  }

  QWC_EQ_I((int)ngot, (int)nexp, "move count == expected");

  Move gs[MG_CAP]; std::memcpy(gs, got, ngot * sizeof(Move));
  sort_moves(gs, ngot); sort_moves(exp, nexp);
  int nodup = 1;
  for (size_t i = 1; i < ngot; i++) if (gs[i] == gs[i - 1]) nodup = 0;
  QWC_TRUE(nodup, "no duplicate moves");
  if (ngot == nexp) {
    int match = 1;
    for (size_t i = 0; i < ngot; i++) if (gs[i] != exp[i]) { match = 0; break; }
    QWC_TRUE(match, "complete encoded Move set matches expected");
  } else {
    QWC_TRUE(0, "set sizes differ (cannot compare)");
  }

  /* flag / payload semantics, checked separately from the coordinate strings */
  int nep = 0, npr = 0, npr_exp = 0;
  for (int i = 0; i < mt; i++) if (std::strlen(mtoks[i]) == 5) npr_exp++;
  for (size_t i = 0; i < ngot; i++) {
    u32 fl = move_flags(got[i]), pr = move_promo(got[i]);
    QWC_TRUE(move_repr_is_valid(got[i]), "no reserved bits set");
    QWC_TRUE(got[i] != 0u, "no null move");
    QWC_TRUE(move_from(got[i]) != move_to(got[i]), "from != to");
    QWC_TRUE((fl & to_under(MoveFlag::MV_CASTLE)) == 0, "no castling in this layer");
    QWC_TRUE((fl & (to_under(MoveFlag::MV_EP) | to_under(MoveFlag::MV_PROMO))) != (to_under(MoveFlag::MV_EP) | to_under(MoveFlag::MV_PROMO)), "no mixed EP|PROMO flags");
    if (fl & to_under(MoveFlag::MV_EP))          { nep++; QWC_EQ_I((int)pr, 0, "EP has zero promo payload"); }
    else if (fl & to_under(MoveFlag::MV_PROMO))  { npr++; QWC_TRUE(pr >= to_under(PieceType::KNIGHT) && pr <= to_under(PieceType::QUEEN), "promo payload in N..Q"); }
    else                     { QWC_EQ_I((int)pr, 0, "quiet move has zero payload"); }
  }
  QWC_EQ_I(nep, et, "MV_EP count == expected EP moves");
  QWC_EQ_I(npr, npr_exp, "MV_PROMO count == expected promo moves");
  return qwc_end();
}

/* ---- the 14 fixtures, original and mirrored ------------------------------- */
static int t_fixtures(void) {
  int fail = 0;
  for (int i = 0; i < NB_FIXTURES; i++) {
    char tag[64];

    /* original position */
    std::snprintf(tag, sizeof tag, "mg.%s", fixtures[i].name);
    qwc_begin(tag);
    Position pos; const char *why = nullptr;
    QWC_TRUE(fen_load(&pos, fixtures[i].fen, &why) == 1, "fixture loads");
    QWC_TRUE(pos_validate(&pos, &why), "fixture validates");
    Move out[MG_CAP]; size_t n = 0;
    QWC_TRUE(movegen_pseudo_noncastling(&pos, out, MG_CAP, &n) == 1, "generation succeeds");
    fail |= cmp_set(out, n, fixtures[i].moves, fixtures[i].ep_moves);

    /* rank-reflected, color-swapped counterpart */
    std::snprintf(tag, sizeof tag, "mg.%s.mirror", fixtures[i].name);
    qwc_begin(tag);
    Position mp; pos_mirror(&pos, &mp);
    QWC_EQ_I((int)mp.side, (int)(1 - to_under(pos.side)), "mirror flips the side");
    QWC_TRUE(pos_validate(&mp, &why), "mirror validates");
    char mref[512], eref[512];
    reflect_moveset(fixtures[i].moves, mref);
    reflect_moveset(fixtures[i].ep_moves, eref);
    Move mout[MG_CAP]; size_t mn = 0;
    QWC_TRUE(movegen_pseudo_noncastling(&mp, mout, MG_CAP, &mn) == 1, "mirror generation succeeds");
    fail |= cmp_set(mout, mn, mref, eref);
  }
  return fail;
}

/* ---- the exact start position -------------------------------------------- */
static int t_startpos(void) {
  qwc_begin("mg.startpos");
  Position pos; pos_set_startpos(&pos);
  Move out[MG_CAP]; size_t n = 0;
  QWC_TRUE(movegen_pseudo_noncastling(&pos, out, MG_CAP, &n) == 1, "generation succeeds");
  static const char *exp =
    "a2a3 b2b3 c2c3 d2d3 e2e3 f2f3 g2g3 h2h3 "
    "a2a4 b2b4 c2c4 d2d4 e2e4 f2f4 g2g4 h2h4 "
    "b1a3 b1c3 g1f3 g1h3";
  return cmp_set(out, n, exp, "");
}

/* Does `got` contain exactly the move from->to with the given flags/payload? */
static int has_move(const Move *got, size_t n, Square from, Square to, u32 flags, u32 promo) {
  for (size_t i = 0; i < n; i++)
    if (move_from(got[i]) == from && move_to(got[i]) == to &&
        move_flags(got[i]) == flags && move_promo(got[i]) == promo)
      return 1;
  return 0;
}

/* Deliberate tests of this layer's boundary: pseudo-legality is NOT legality.
 * A king may move to an attacked square and a pinned captor's en-passant still
 * stands, because own-king safety is filtered later (cp2), not here. */
static int t_boundary(void) {
  int fail = 0;
  /* king in check: e1d2 (attacked) and e1e2 (captures the checking rook) both
   * remain candidates. */
  qwc_begin("mg.boundary.check");
  {
    Position pos; const char *why = nullptr;
    QWC_TRUE(fen_load(&pos, "7k/8/8/8/8/8/4r3/4K3 w - - 0 1", &why) == 1, "load");
    Move out[MG_CAP]; size_t n = 0;
    QWC_TRUE(movegen_pseudo_noncastling(&pos, out, MG_CAP, &n) == 1, "gen");
    Square e1 = square_of(4, 0), d2 = square_of(3, 1), e2 = square_of(4, 1);
    QWC_TRUE(has_move(out, n, e1, d2, 0u, 0u), "e1d2 present (attacked; king may move there)");
    QWC_TRUE(has_move(out, n, e1, e2, 0u, 0u), "e1e2 present (captures the checking rook)");
  }
  fail |= qwc_end();

  /* pinned / mixed ep: e5d6 (an EP capture that exposes the king) remains a
   * candidate in both fixtures. */
  qwc_begin("mg.boundary.ep_pin");
  {
    static const char *fens[2] = {
      "k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 20",  /* pinned_ep */
      "k3r3/8/8/2PpP3/8/8/8/4K3 w - d6 0 20",  /* mixed_ep  */
    };
    Square e5 = square_of(4, 4), d6 = square_of(3, 5);
    for (int k = 0; k < 2; k++) {
      Position pos; const char *why = nullptr;
      QWC_TRUE(fen_load(&pos, fens[k], &why) == 1, "load");
      Move out[MG_CAP]; size_t n = 0;
      QWC_TRUE(movegen_pseudo_noncastling(&pos, out, MG_CAP, &n) == 1, "gen");
      QWC_TRUE(has_move(out, n, e5, d6, (u32)MoveFlag::MV_EP, 0u), "e5d6 (EP) present despite pin/king exposure");
    }
  }
  fail |= qwc_end();
  return fail;
}

/* A pawn whose FIRST step is blocked: blocking the intermediate square must
 * remove both the single and the double push (the d2 pawn faces a knight on d3).
 * Here only the king's three moves remain. */
static int t_blocked_first(void) {
  qwc_begin("mg.blocked_first");
  Position pos; const char *why = nullptr;
  QWC_TRUE(fen_load(&pos, "7k/8/8/8/8/3n4/3P4/7K w - - 0 1", &why) == 1, "load");
  QWC_TRUE(pos_validate(&pos, &why), "validates");
  Move out[MG_CAP]; size_t n = 0;
  QWC_TRUE(movegen_pseudo_noncastling(&pos, out, MG_CAP, &n) == 1, "gen");
  static const char *exp = "h1g1 h1g2 h1h2";
  return cmp_set(out, n, exp, "");
}

/* Field-by-field equality of two undo records (prev_* scalars + the move delta). */
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

/* ---- capacity / overflow -------------------------------------------------- */
static int t_capacity(void) {
  int fail = 0;
  Position pos; pos_set_startpos(&pos);   /* exactly 20 moves */

  qwc_begin("mg.capacity");
  {
    Move buf[MG_CAP]; size_t n = 99;
    QWC_TRUE(movegen_pseudo_noncastling(&pos, buf, 20, &n) == 1, "cap 20 succeeds");
    QWC_EQ_I((int)n, 20, "cap 20 writes exactly 20");
    n = 99;
    QWC_TRUE(movegen_pseudo_noncastling(&pos, buf, 19, &n) == 0, "cap 19 overflows -> fail");
    QWC_EQ_I((int)n, 0, "cap 19 count reset to zero");
    n = 99;
    QWC_TRUE(movegen_pseudo_noncastling(&pos, buf, 0, &n) == 0, "cap 0 (non-empty) -> fail");
    QWC_EQ_I((int)n, 0, "cap 0 count reset to zero");
    QWC_TRUE(movegen_pseudo_noncastling(&pos, buf, 20, nullptr) == 1, "cap 20 with count nullptr succeeds");
    QWC_TRUE(movegen_pseudo_noncastling(&pos, nullptr, 20, nullptr) == 0, "nullptr out fails");
    QWC_TRUE(movegen_pseudo_noncastling(nullptr, buf, 20, nullptr) == 0, "nullptr pos fails");
  }
  fail |= qwc_end();

  /* canary: on a 19-capacity overflow nothing may be written beyond index 18 */
  qwc_begin("mg.capacity.canary");
  {
    const Move sent = (Move)0xA5A5A5A5u;
    Move cbuf[MG_CAP];
    for (int i = 0; i < MG_CAP; i++) cbuf[i] = sent;
    size_t n = 99;
    movegen_pseudo_noncastling(&pos, cbuf, 19, &n);
    int beyond_ok = 1;
    for (int i = 19; i < MG_CAP; i++) if (cbuf[i] != sent) beyond_ok = 0;
    QWC_TRUE(beyond_ok, "no write beyond capacity (canary untouched)");
  }
  fail |= qwc_end();
  return fail;
}

/* ---- determinism + no mutation + live history prefix ---------------------- */
static int t_determinism(void) {
  int fail = 0;

  qwc_begin("mg.determinism.repeat");
  {
    Position pos; pos_set_startpos(&pos);
    Position before = pos;
    Move a[MG_CAP], b[MG_CAP]; size_t na = 0, nb = 0;
    QWC_TRUE(movegen_pseudo_noncastling(&pos, a, MG_CAP, &na) == 1, "gen 1");
    QWC_TRUE(movegen_pseudo_noncastling(&pos, b, MG_CAP, &nb) == 1, "gen 2");
    QWC_EQ_I((int)na, (int)nb, "same count on repeat");
    int same = 1;
    for (size_t i = 0; i < na; i++) if (a[i] != b[i]) same = 0;
    QWC_TRUE(same, "same sequence on repeat");
    QWC_TRUE(pos_equal(&pos, &before), "generation leaves Position untouched");
    QWC_TRUE(pos.history == before.history, "history link untouched");
  }
  fail |= qwc_end();

  qwc_begin("mg.determinism.history");
  {
    /* a live caller-owned history prefix (two structural makes) is left untouched
     * and generation does not depend on it */
    Position p2; pos_set_startpos(&p2);
    StateInfo st[2]; std::memset(st, 0, sizeof st);
    QWC_TRUE(pos_make_move(&p2, &st[0], make_move(square_of(4, 1), square_of(4, 3), 0u, 0u)) == 1, "make 1.e2e4");
    QWC_TRUE(pos_make_move(&p2, &st[1], make_move(square_of(3, 6), square_of(3, 4), 0u, 0u)) == 1, "make 1...d7d5");
    QWC_TRUE(p2.history == &st[1] && st[1].prev == &st[0] && st[0].prev == nullptr, "live 2-record history chain");
    Position before2 = p2;
    StateInfo head_snap = st[1];
    Move out[MG_CAP]; size_t n = 0;
    QWC_TRUE(movegen_pseudo_noncastling(&p2, out, MG_CAP, &n) == 1, "gen");
    QWC_TRUE(pos_equal(&p2, &before2), "Position untouched (live history)");
    QWC_TRUE(p2.history == &st[1], "history head untouched");
    QWC_TRUE(state_equal(&st[1], &head_snap), "head record byte-identical");
    Move out2[MG_CAP]; size_t n2 = 0;
    QWC_TRUE(movegen_pseudo_noncastling(&p2, out2, MG_CAP, &n2) == 1, "gen again");
    QWC_EQ_I((int)n, (int)n2, "same count on repeat");
    int same = 1;
    for (size_t i = 0; i < n; i++) if (out[i] != out2[i]) same = 0;
    QWC_TRUE(same, "identical on repeat (live history)");
  }
  fail |= qwc_end();
  return fail;
}

/* ---- counter independence ------------------------------------------------- */
/* The same non-EP board at normal (0/1) and at 65535/65535 counters must yield
 * the identical candidate set: rule-50/fullmove do not affect pseudo-legality. */
static int t_counters(void) {
  qwc_begin("mg.counters");
  const char *fen = "7k/8/8/8/8/3p4/3R4/K7 w - - 0 1";   /* the rook fixture; no EP */
  Position a, b; const char *why = nullptr;
  QWC_TRUE(fen_load(&a, fen, &why) == 1, "load A");
  QWC_TRUE(fen_load(&b, fen, &why) == 1, "load B");
  a.halfmove = 0;     a.fullmove = 1;
  b.halfmove = 65535; b.fullmove = 65535;
  pos_rebuild(&a); pos_rebuild(&b);   /* the key/caches do not depend on counters */
  Move oa[MG_CAP], ob[MG_CAP]; size_t na = 0, nb = 0;
  QWC_TRUE(movegen_pseudo_noncastling(&a, oa, MG_CAP, &na) == 1, "gen A");
  QWC_TRUE(movegen_pseudo_noncastling(&b, ob, MG_CAP, &nb) == 1, "gen B");
  QWC_EQ_I((int)na, (int)nb, "same count at normal vs max counters");
  Move sa[MG_CAP], sb[MG_CAP];
  std::memcpy(sa, oa, na * sizeof(Move)); std::memcpy(sb, ob, nb * sizeof(Move));
  sort_moves(sa, na); sort_moves(sb, nb);
  int same = 1;
  for (size_t i = 0; i < na; i++) if (sa[i] != sb[i]) same = 0;
  QWC_TRUE(same, "identical candidate set at normal vs max counters");
  return qwc_end();
}

/* ---- per-candidate apply/unmake round-trip -------------------------------- */
/* Apply EVERY generated candidate with the T004 make/unmake APIs. Each apply
 * must be structurally successful; the child's derived caches + key must match a
 * cold rebuild; and the unmake must restore the exact logical/derived state and
 * the history link. An unsafe pseudo-legal child need not pass pos_validate /
 * fen_emit, so those are deliberately NOT asserted (cp2 territory). */
static int t_apply_unapply(void) {
  int fail = 0;
  static const char *fens[] = {
    "7k/8/8/8/8/3p4/3R4/K7 w - - 0 1",     /* rook: quiet + slider capture      */
    "1r5k/P7/8/8/8/8/8/7K w - - 0 1",         /* promotion: pushes + capture-promos */
    "k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 20", /* pinned_ep: includes the EP   */
    "7k/8/8/8/8/8/4r3/4K3 w - - 0 1",         /* king_in_check: unsafe children   */
  };
  enum { NF = (int)(sizeof fens / sizeof fens[0]) };
  for (int k = 0; k < NF; k++) {
    char tag[64]; std::snprintf(tag, sizeof tag, "mg.apply.%d", k);
    qwc_begin(tag);
    Position pos; const char *why = nullptr; const char *w = nullptr;
    QWC_TRUE(fen_load(&pos, fens[k], &why) == 1, "load");
    Move out[MG_CAP]; size_t n = 0;
    QWC_TRUE(movegen_pseudo_noncastling(&pos, out, MG_CAP, &n) == 1, "gen");
    QWC_TRUE(n > 0, "non-empty");
    Position base = pos;
    int nmake = 0;
    for (size_t i = 0; i < n; i++) {
      Position before = pos;
      StateInfo st; std::memset(&st, 0, sizeof st);
      QWC_TRUE(pos_make_move(&pos, &st, out[i]) == 1, "structural make succeeds");
      QWC_TRUE(derived_match_rebuild(&pos, &w), "child caches/key == cold rebuild");
      pos_unmake_move(&pos, &st);
      QWC_TRUE(pos_equal(&pos, &before), "unmake restores Position exactly");
      QWC_TRUE(pos.history == before.history, "history link restored");
      nmake++;
    }
    QWC_EQ_I(nmake, (int)n, "every candidate round-tripped");
    QWC_TRUE(pos_equal(&pos, &base), "board returned to the loaded state");
    fail |= qwc_end();
  }
  return fail;
}

int main(void) {
  int fail = 0;
  fail |= t_fixtures();
  fail |= t_startpos();
  fail |= t_boundary();
  fail |= t_blocked_first();
  fail |= t_capacity();
  fail |= t_determinism();
  fail |= t_counters();
  fail |= t_apply_unapply();
  std::printf("\nmovegen pseudo-legal: %s\n", fail ? "FAIL" : "all groups PASS");
  return fail ? 1 : 0;
}
