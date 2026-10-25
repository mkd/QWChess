/* QwenChess T005 checkpoint-2 test: non-castling LEGAL move generation.
 *
 * Verifies movegen_legal_noncastling() (src/movegen/movegen.cpp) against the
 * independently-derived LEGAL move sets. "Legal" = pseudo-legal AND leaving the
 * original mover's own king unattacked on the resulting board (a giving check is
 * legal). The fixed corpus below is the complete non-castling legal set per
 * position, plus its rank-reflected / color-swapped mirror.
 *
 * FIXTURE PROVENANCE (prompt-supplied): the expected sets were computed
 * independently, at prompt-preparation time, with the python `chess` module
 * 1.11.2 (`Board.legal_moves` filtered by `not board.is_castling(move)`; the EP
 * subset by `board.is_en_passant(move)`); all 22 positions passed that library's
 * validity check and their rank/color mirrors produced the reflected sets. That is
 * preparatory, prompt-supplied provenance -- it is NOT proof of full historical
 * reachability, and this build does not re-run python-chess (no Python dependency
 * in the engine/test build). The coordinate strings are test-only notation; a small
 * decoder turns them into Moves. This is NOT production UCI parsing.
 *
 * Groups: the 22 fixtures and their mirrors (complete encoded Move set + the EP /
 * promotion / no-castling / no-null / no-duplicate flag semantics); capacity with
 * canaries (incl. mate/stalemate succeeding on an empty span); preservation and
 * determinism (the Position -- board, key, counters, history head and live records
 * -- is untouched on success, overflow and zero-result calls); counter
 * independence (u16 boundaries, both colors, EP retains halfmove 0); and an
 * integration round-trip (each emitted move makes, the child is consistent and
 * matches a cold rebuild, and unmake restores all logical state + history).
 *
 * These are focused cp2 regressions; they are not a claim that the full cp4
 * perft / random-differential milestone is complete.
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
#include <array>
#include <algorithm>
#include <span>

using namespace qwc;

/* Test output buffer, larger than any fixture's legal-move count. */
enum { MGL_CAP = 256 };

/* ---- small test-only helpers (mirror the pseudo-test; no harness refactor) --- */

/* Decode a coordinate string ("a1a2" or "a7a8q") into a Move. A promotion suffix
 * (n/b/r/q) sets MoveFlag::MV_PROMO with the matching payload. Returns 0 if bad. */
static int coord_to_move(const char *s, Move *out) {
  std::size_t len = std::strlen(s);
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

/* Reflect a square: same file, rank index 7-r. */
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
 * reflected, the rights' color bits swapped, the history abandoned. */
static void pos_mirror(const Position *src, Position *dst) {
  dst->mailbox = {};
  for (int s = 0; s < SQ_NB; s++) {
    int p = to_under(src->mailbox[s]);
    if (!p) continue;
    dst->mailbox[square_of(file_of((Square)s), 7 - rank_of((Square)s))] = (Piece)(p ^ 8);
  }
  dst->side     = (Color)(1 - to_under(src->side));
  dst->cr       = mirror_cr(src->cr);
  dst->ep_sq    = (src->ep_sq < SQ_NB) ? reflect_sq(src->ep_sq) : (Square)NO_SQUARE;
  dst->halfmove = src->halfmove;
  dst->fullmove = src->fullmove;
  dst->history  = nullptr;
  pos_rebuild(dst);
}

/* Field-by-field equality over the ENTIRE Position INCLUDING the history pointer
 * (the pseudo-test helper omits it; the legal API must not touch it). */
static int pos_equal_full(const Position *a, const Position *b) {
  for (int s = 0; s < SQ_NB; s++) if (a->mailbox[s] != b->mailbox[s]) return 0;
  if (a->side != b->side) return 0;
  if (a->cr != b->cr) return 0;
  if (a->ep_sq != b->ep_sq) return 0;
  if (a->halfmove != b->halfmove) return 0;
  if (a->fullmove != b->fullmove) return 0;
  for (int p = 0; p < PIECE_NB; p++) if (a->by_piece[p] != b->by_piece[p]) return 0;
  for (int c = 0; c < COLOR_NB; c++) if (a->by_color[c] != b->by_color[c]) return 0;
  if (a->occ != b->occ) return 0;
  if (a->white_king_sq != b->white_king_sq) return 0;
  if (a->black_king_sq != b->black_king_sq) return 0;
  if (a->key != b->key) return 0;
  if (a->history != b->history) return 0;
  return 1;
}

/* Do the derived caches + key match a from-scratch rebuild of the same board? */
static int derived_match_rebuild(const Position *pos, const char **why) {
  Position copy = *pos;
  pos_rebuild(&copy);
  for (int p = 0; p < PIECE_NB; p++) if (pos->by_piece[p] != copy.by_piece[p]) { if (why) *why = "by_piece mismatch vs rebuild"; return 0; }
  for (int c = 0; c < COLOR_NB; c++) if (pos->by_color[c] != copy.by_color[c]) { if (why) *why = "by_color mismatch vs rebuild"; return 0; }
  if (pos->occ != copy.occ) { if (why) *why = "occ mismatch vs rebuild"; return 0; }
  if (pos->white_king_sq != copy.white_king_sq || pos->black_king_sq != copy.black_king_sq) { if (why) *why = "king square mismatch vs rebuild"; return 0; }
  if (pos->key != copy.key) { if (why) *why = "key mismatch vs rebuild"; return 0; }
  return 1;
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

/* Copy a NUL-terminated string into `dst`, clamped to `cap`. */
static void strcopy(char *dst, std::size_t cap, const char *src) {
  std::size_t n = std::strlen(src);
  if (n >= cap) n = cap - 1;
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

/* Reflect every coordinate in a space-separated moveset string (rank 9-rank; files
 * and any promotion suffix preserved). */
static void reflect_moveset(const char *src, char dst[512]) {
  char buf[512]; strcopy(buf, sizeof buf, src);
  char *toks[128]; int nt = split_spaces(buf, toks, 128);
  std::size_t di = 0;
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
static void sort_moves(Move *a, std::size_t n) {
  for (std::size_t i = 1; i < n; i++) {
    Move key = a[i]; std::size_t j = i;
    while (j > 0 && a[j - 1] > key) { a[j] = a[j - 1]; j--; }
    a[j] = key;
  }
}

/* Move -> a short "a1a2" style string for diagnostics (promo suffix, EP tag). */
static void move_to_coord(Move m, char out[8]) {
  out[0] = (char)('a' + file_of(move_from(m)));
  out[1] = (char)('1' + rank_of(move_from(m)));
  out[2] = (char)('a' + file_of(move_to(m)));
  out[3] = (char)('1' + rank_of(move_to(m)));
  std::size_t k = 4;
  if (move_flags(m) & to_under(MoveFlag::MV_EP)) out[k++] = 'e';
  else if (move_flags(m) & to_under(MoveFlag::MV_PROMO)) {
    switch ((PieceType)move_promo(m)) {
      case PieceType::KNIGHT: out[k++] = 'n'; break;
      case PieceType::BISHOP: out[k++] = 'b'; break;
      case PieceType::ROOK:   out[k++] = 'r'; break;
      default:                out[k++] = 'q'; break;
    }
  }
  out[k] = '\0';
}

/* Compare the generated `got`/`ngot` against the expected coord set. Verifies the
 * complete encoded Move set (order-independent after sorting), no duplicates, and --
 * separately -- the flag/payload semantics (MV_EP count, MV_PROMO count + range, no
 * castling, no mixed flags, no reserved bits, from != to, no null). On a content
 * mismatch it prints the moves in each set so the exact discrepancy is visible. */
static int cmp_set(const char *tag, const Move *got, std::size_t ngot,
                   const char *moves, const char *ep_list) {
  std::array<Move, 128> exp; std::size_t nexp = 0;
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

  Move gs[MGL_CAP]; std::memcpy(gs, got, ngot * sizeof(Move));
  sort_moves(gs, ngot); sort_moves(exp.data(), nexp);
  int nodup = 1;
  for (std::size_t i = 1; i < ngot; i++) if (gs[i] == gs[i - 1]) nodup = 0;
  QWC_TRUE(nodup, "no duplicate moves");
  if (ngot == nexp) {
    int match = 1;
    for (std::size_t i = 0; i < ngot; i++) if (gs[i] != exp[i]) { match = 0; break; }
    QWC_TRUE(match, "complete encoded Move set matches expected");
    if (!match) {   /* show the exact discrepancy (for the handoff report) */
      std::printf("    [%s] GOT (%zu):", tag, ngot);
      for (std::size_t i = 0; i < ngot; i++) { char c[8]; move_to_coord(gs[i], c); std::printf(" %s", c); }
      std::printf("\n    [%s] EXP (%zu):", tag, nexp);
      for (std::size_t i = 0; i < nexp; i++) { char c[8]; move_to_coord(exp[i], c); std::printf(" %s", c); }
      std::printf("\n");
    }
  } else {
    QWC_TRUE(0, "set sizes differ (cannot compare)");
  }

  /* flag / payload semantics, checked separately from the coordinate strings */
  int nep = 0, npr = 0, npr_exp = 0;
  for (int i = 0; i < mt; i++) if (std::strlen(mtoks[i]) == 5) npr_exp++;
  for (std::size_t i = 0; i < ngot; i++) {
    u32 fl = move_flags(got[i]), pr = move_promo(got[i]);
    QWC_TRUE(move_repr_is_valid(got[i]), "no reserved bits set");
    QWC_TRUE(got[i] != 0u, "no null move");
    QWC_TRUE(move_from(got[i]) != move_to(got[i]), "from != to");
    QWC_TRUE((fl & to_under(MoveFlag::MV_CASTLE)) == 0, "no castling in this API");
    QWC_TRUE((fl & (to_under(MoveFlag::MV_EP) | to_under(MoveFlag::MV_PROMO))) !=
             (to_under(MoveFlag::MV_EP) | to_under(MoveFlag::MV_PROMO)), "no mixed EP|PROMO flags");
    if (fl & to_under(MoveFlag::MV_EP))          { nep++; QWC_EQ_I((int)pr, 0, "EP has zero promo payload"); }
    else if (fl & to_under(MoveFlag::MV_PROMO))  { npr++; QWC_TRUE(pr >= to_under(PieceType::KNIGHT) && pr <= to_under(PieceType::QUEEN), "promo payload in N..Q"); }
    else                     { QWC_EQ_I((int)pr, 0, "quiet move has zero payload"); }
  }
  QWC_EQ_I(nep, et, "MV_EP count == expected EP moves");
  QWC_EQ_I(npr, npr_exp, "MV_PROMO count == expected promo moves");
  return qwc_end();
}

/* ---- the fixed corpus (prompt-supplied provenance; see the file header) ------- */
typedef struct {
  const char *name;
  const char *fen;
  const char *moves;   /* complete expected LEGAL set (space-separated coords) */
  const char *ep;      /* the subset carrying MoveFlag::MV_EP ("" = none) */
} Fixture;

static const Fixture fixtures[] = {
  { "startpos",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "a2a3 a2a4 b1a3 b1c3 b2b3 b2b4 c2c3 c2c4 d2d3 d2d4 e2e3 e2e4 "
    "f2f3 f2f4 g1f3 g1h3 g2g3 g2g4 h2h3 h2h4", "" },
  { "pin_along_ray",
    "k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1",
    "e1d1 e1d2 e1f1 e1f2 e2e3 e2e4 e2e5 e2e6 e2e7 e2e8", "" },
  { "quiet_block",
    "k3r3/8/8/8/8/8/8/4KB2 w - - 0 1",
    "e1d1 e1d2 e1f2 f1e2", "" },
  { "knight_check_capture",
    "k7/8/8/8/8/5n2/6P1/4K3 w - - 0 1",
    "e1d1 e1e2 e1f1 e1f2 g2f3", "" },
  { "double_check",
    "k3r3/8/8/8/1b6/8/2N5/4K3 w - - 0 1",
    "e1d1 e1f1 e1f2", "" },
  { "pinned_enemy_knight",
    "4k3/4n3/8/8/2K5/8/8/4R3 w - - 0 1",
    "c4b3 c4b4 c4b5 c4c3 c4c5 c4d3 c4d4 e1a1 e1b1 e1c1 e1d1 "
    "e1e2 e1e3 e1e4 e1e5 e1e6 e1e7 e1f1 e1g1 e1h1", "" },
  { "king_capture_xray",
    "k3r3/8/8/8/8/8/4n3/4K3 w - - 0 1",
    "e1d1 e1d2 e1f1 e1f2", "" },
  { "king_vacates_ray",
    "k3r3/8/8/8/8/8/4K3/8 w - - 0 1",
    "e2d1 e2d2 e2d3 e2f1 e2f2 e2f3", "" },
  { "king_adjacency",
    "8/8/8/8/8/4k3/8/4K3 w - - 0 1",
    "e1d1 e1f1", "" },
  { "promotion_capture_evasion",
    "1r5k/P7/8/8/8/8/8/1K6 w - - 0 1",
    "a7b8b a7b8n a7b8q a7b8r b1a1 b1a2 b1c1 b1c2", "" },
  { "promotion_block",
    "r6K/5P2/8/8/8/8/8/k7 w - - 0 1",
    "f7f8b f7f8n f7f8q f7f8r h8g7 h8h7", "" },
  { "checkmate",
    "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1",
    "", "" },
  { "stalemate",
    "7k/5K2/6Q1/8/8/8/8/8 b - - 0 1",
    "", "" },
  { "castling_excluded",
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1d1 e1d2 "
    "e1e2 e1f1 e1f2 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8", "" },
  { "ep_1",
    "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20",
    "e1d1 e1d2 e1e2 e1f1 e1f2 e5d6 e5e6", "e5d6" },
  { "ep_2",
    "k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 20",
    "e1d1 e1d2 e1e2 e1f1 e1f2 e5e6", "" },
  { "ep_3",
    "k7/8/8/r4pPK/8/8/8/8 w - f6 0 20",
    "g5g6 h5g6 h5h4 h5h6", "" },
  { "ep_4",
    "k7/8/8/3pP3/4K3/8/8/8 w - d6 0 20",
    "e4d3 e4d4 e4d5 e4e3 e4f3 e4f4 e4f5 e5d6", "e5d6" },
  { "ep_5",
    "k3r3/8/8/2PpP3/8/8/8/4K3 w - d6 0 20",
    "c5c6 c5d6 e1d1 e1d2 e1e2 e1f1 e1f2 e5e6", "c5d6" },
  { "ep_6",
    "k7/8/8/3p4/8/8/8/4K3 w - d6 0 20",
    "e1d1 e1d2 e1e2 e1f1 e1f2", "" },
  { "ep_7",
    "kb6/8/8/4Pp2/8/8/7K/8 w - f6 0 20",
    "h2g1 h2g2 h2g3 h2h1 h2h3", "" },
  { "ep_8",
    "4b2k/8/8/3pP3/K7/8/8/8 w - d6 0 20",
    "a4a3 a4a5 a4b3 a4b4", "" },
};
enum { NB_FIXTURES = (int)(sizeof fixtures / sizeof fixtures[0]) };

/* ---- the 22 fixtures, original and mirrored ---------------------------------- */
static int t_fixtures() {
  int fail = 0;
  for (int i = 0; i < NB_FIXTURES; i++) {
    std::array<char, 48> tag{};

    /* original position */
    std::snprintf(tag.data(), tag.size(), "mgl.%s", fixtures[i].name);
    qwc_begin(tag.data());
    Position pos; const char *why = nullptr;
    QWC_TRUE(fen_load(&pos, fixtures[i].fen, &why) == 1, "fixture loads");
    QWC_TRUE(pos_validate(&pos, &why), "fixture validates");
    Move out[MGL_CAP]; std::size_t n = 12345;
    QWC_TRUE(movegen_legal_noncastling(pos, std::span<Move>(out, MGL_CAP), n), "generation succeeds");
    fail |= cmp_set(tag.data(), out, n, fixtures[i].moves, fixtures[i].ep);

    /* rank-reflected, color-swapped counterpart */
    std::snprintf(tag.data(), tag.size(), "mgl.%s.mirror", fixtures[i].name);
    qwc_begin(tag.data());
    Position mp; pos_mirror(&pos, &mp);
    QWC_EQ_I((int)mp.side, (int)(1 - to_under(pos.side)), "mirror flips the side");
    QWC_TRUE(pos_validate(&mp, &why), "mirror validates");
    std::array<char, 512> mref{}, eref{};
    reflect_moveset(fixtures[i].moves, mref.data());
    reflect_moveset(fixtures[i].ep, eref.data());
    Move mout[MGL_CAP]; std::size_t mn = 12345;
    QWC_TRUE(movegen_legal_noncastling(mp, std::span<Move>(mout, MGL_CAP), mn), "mirror generation succeeds");
    fail |= cmp_set(tag.data(), mout, mn, mref.data(), eref.data());
  }
  return fail;
}

/* ---- capacity / overflow ----------------------------------------------------- */
static int t_capacity() {
  int fail = 0;

  /* startpos: exactly 20 legal moves. cap 20 succeeds; 19 and 0 fail/count 0. */
  qwc_begin("mgl.capacity.startpos");
  {
    Position pos; pos_set_startpos(&pos);
    Move buf[MGL_CAP]; std::size_t n = 12345;
    QWC_TRUE(movegen_legal_noncastling(pos, std::span<Move>(buf, 20), n), "cap 20 succeeds");
    QWC_EQ_I((int)n, 20, "cap 20 writes exactly 20");
    n = 12345;
    QWC_TRUE(!movegen_legal_noncastling(pos, std::span<Move>(buf, 19), n), "cap 19 overflows");
    QWC_EQ_I((int)n, 0, "cap 19 count reset to 0");
    n = 12345;
    QWC_TRUE(!movegen_legal_noncastling(pos, std::span<Move>(buf, 0), n), "cap 0 (non-empty) fails");
    QWC_EQ_I((int)n, 0, "cap 0 count reset to 0");
  }
  fail |= qwc_end();

  /* quiet_block: 4 legal moves out of 11 pseudo-legal candidates. cap 4 succeeds;
   * cap 3 fails. */
  qwc_begin("mgl.capacity.quiet_block");
  {
    Position pos; const char *why = nullptr;
    QWC_TRUE(fen_load(&pos, "k3r3/8/8/8/8/8/8/4KB2 w - - 0 1", &why) == 1, "load");
    Move buf[MGL_CAP]; std::size_t n = 12345;
    QWC_TRUE(movegen_legal_noncastling(pos, std::span<Move>(buf, 4), n), "cap 4 succeeds (4 of 11 candidates)");
    QWC_EQ_I((int)n, 4, "cap 4 writes exactly 4");
    n = 12345;
    QWC_TRUE(!movegen_legal_noncastling(pos, std::span<Move>(buf, 3), n), "cap 3 overflows");
    QWC_EQ_I((int)n, 0, "cap 3 count reset to 0");
  }
  fail |= qwc_end();

  /* mate / stalemate: zero legal moves -> an empty span SUCCEEDS with count 0. */
  qwc_begin("mgl.capacity.empty_result");
  {
    static const char *fens[2] = {
      "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1",   /* checkmate */
      "7k/5K2/6Q1/8/8/8/8/8 b - - 0 1",   /* stalemate */
    };
    for (int k = 0; k < 2; k++) {
      Position pos; const char *why = nullptr;
      QWC_TRUE(fen_load(&pos, fens[k], &why) == 1, "load");
      Move buf[MGL_CAP];
      for (int i = 0; i < MGL_CAP; i++) buf[i] = (Move)0xA5A5A5A5u;   /* canary */
      std::size_t n = 12345;
      QWC_TRUE(movegen_legal_noncastling(pos, std::span<Move>(buf, 0), n), "empty span succeeds (no legal moves)");
      QWC_EQ_I((int)n, 0, "count is 0 (no legal moves)");
      int canary = 1;
      for (int i = 0; i < MGL_CAP; i++) if (buf[i] != (Move)0xA5A5A5A5u) canary = 0;
      QWC_TRUE(canary, "zero-size span writes nothing (canary untouched)");
    }
  }
  fail |= qwc_end();

  /* canary: a 19-capacity overflow on startpos leaves every index >= 19 untouched. */
  qwc_begin("mgl.capacity.canary");
  {
    Position pos; pos_set_startpos(&pos);
    Move cbuf[MGL_CAP];
    for (int i = 0; i < MGL_CAP; i++) cbuf[i] = (Move)0xA5A5A5A5u;
    std::size_t n = 12345;
    QWC_TRUE(!movegen_legal_noncastling(pos, std::span<Move>(cbuf, 19), n), "19-cap overflow fails");
    QWC_EQ_I((int)n, 0, "overflow count reset to 0");
    int beyond_ok = 1;
    for (int i = 19; i < MGL_CAP; i++) if (cbuf[i] != (Move)0xA5A5A5A5u) beyond_ok = 0;
    QWC_TRUE(beyond_ok, "no write beyond the 19-capacity span (canary untouched)");
  }
  fail |= qwc_end();
  return fail;
}

/* ---- preservation / determinism --------------------------------------------- */
static int t_preservation() {
  int fail = 0;

  /* repeated calls give the same sequence; the whole Position (incl. history) is
   * untouched on a successful, an overflowing and a zero-result call. */
  qwc_begin("mgl.preservation.repeat");
  {
    Position pos; pos_set_startpos(&pos);
    Position before = pos;
    Move a[MGL_CAP], b[MGL_CAP]; std::size_t na = 0, nb = 0;
    QWC_TRUE(movegen_legal_noncastling(pos, std::span<Move>(a, MGL_CAP), na), "gen 1");
    QWC_TRUE(movegen_legal_noncastling(pos, std::span<Move>(b, MGL_CAP), nb), "gen 2");
    QWC_EQ_I((int)na, (int)nb, "same count on repeat");
    int same = 1;
    for (std::size_t i = 0; i < na; i++) if (a[i] != b[i]) same = 0;
    QWC_TRUE(same, "same sequence on repeat");
    QWC_TRUE(pos_equal_full(&pos, &before), "Position untouched (incl. history) on success");
  }
  fail |= qwc_end();

  qwc_begin("mgl.preservation.overflow");
  {
    Position pos; pos_set_startpos(&pos);
    Position before = pos;
    Move buf[MGL_CAP]; std::size_t n = 12345;
    QWC_TRUE(!movegen_legal_noncastling(pos, std::span<Move>(buf, 19), n), "overflow");
    QWC_TRUE(pos_equal_full(&pos, &before), "Position untouched (incl. history) on overflow");
  }
  fail |= qwc_end();

  qwc_begin("mgl.preservation.zero");
  {
    Position pos; const char *why = nullptr;
    QWC_TRUE(fen_load(&pos, "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1", &why) == 1, "load (checkmate)");
    Position before = pos;
    Move buf[MGL_CAP]; std::size_t n = 12345;
    QWC_TRUE(movegen_legal_noncastling(pos, std::span<Move>(buf, MGL_CAP), n), "gen (zero result)");
    QWC_EQ_I((int)n, 0, "zero legal moves");
    QWC_TRUE(pos_equal_full(&pos, &before), "Position untouched (incl. history) on zero result");
  }
  fail |= qwc_end();

  /* a live caller-owned history prefix (two structural makes) is left untouched and
   * the live records are byte-identical after the call; the API never returns a
   * pointer to a local record. */
  qwc_begin("mgl.preservation.history");
  {
    Position p2; pos_set_startpos(&p2);
    std::array<StateInfo, 2> st{};
    QWC_TRUE(pos_make_move(&p2, &st[0], make_move(square_of(4, 1), square_of(4, 3), 0u, 0u)) == 1, "make 1.e2e4");
    QWC_TRUE(pos_make_move(&p2, &st[1], make_move(square_of(3, 6), square_of(3, 4), 0u, 0u)) == 1, "make 1...d7d5");
    QWC_TRUE(p2.history == &st[1] && st[1].prev == &st[0] && st[0].prev == nullptr, "live 2-record history chain");
    Position before2 = p2;
    StateInfo head_snap = st[1];
    Move out[MGL_CAP]; std::size_t n = 0;
    QWC_TRUE(movegen_legal_noncastling(p2, std::span<Move>(out, MGL_CAP), n), "gen (live history)");
    QWC_TRUE(pos_equal_full(&p2, &before2), "Position untouched (incl. history) with live history");
    QWC_TRUE(p2.history == &st[1], "history head still points at the caller's record");
    QWC_TRUE(state_equal(&st[1], &head_snap), "head record byte-identical");
    QWC_TRUE(state_equal(&st[0], &st[0]), "earlier record still self-consistent");
  }
  fail |= qwc_end();
  return fail;
}

/* ---- counter independence ---------------------------------------------------- */
/* The same non-EP board at ordinary counters and at the u16 boundary must yield the
 * identical legal set (rule-50 / fullmove do not affect legality); an EP board keeps
 * its required halfmove 0 but exercises the maximum fullmove. Actual game make() must
 * STILL reject a counter overflow -- only the scratch probe normalizes. */
static int t_counters() {
  int fail = 0;

  static const char *fens[3] = {
    "7k/8/8/8/8/3p4/3R4/K7 w - - 0 1",   /* rook: quiet + slider, no EP (white) */
    "4k3/4n3/8/8/2K5/8/8/4R3 w - - 0 1",   /* pinned knight: many (white) */
    "r6K/5P2/8/8/8/8/8/k7 w - - 0 1",   /* promotion block (white) */
  };
  for (int k = 0; k < 3; k++) {
    char tag[48]; std::snprintf(tag, sizeof tag, "mgl.counters.n.ep.%d", k);
    qwc_begin(tag);
    Position a, b; const char *why = nullptr;
    QWC_TRUE(fen_load(&a, fens[k], &why) == 1, "load A");
    QWC_TRUE(fen_load(&b, fens[k], &why) == 1, "load B");
    a.halfmove = 0;     a.fullmove = 1;      pos_rebuild(&a);
    b.halfmove = 65535; b.fullmove = 65535;  pos_rebuild(&b);
    Move oa[MGL_CAP], ob[MGL_CAP]; std::size_t na = 0, nb = 0;
    QWC_TRUE(movegen_legal_noncastling(a, std::span<Move>(oa, MGL_CAP), na), "gen A");
    QWC_TRUE(movegen_legal_noncastling(b, std::span<Move>(ob, MGL_CAP), nb), "gen B");
    QWC_EQ_I((int)na, (int)nb, "same count at normal vs u16-max counters");
    Move sa[MGL_CAP], sb[MGL_CAP];
    std::memcpy(sa, oa, na * sizeof(Move)); std::memcpy(sb, ob, nb * sizeof(Move));
    sort_moves(sa, na); sort_moves(sb, nb);
    int same = 1;
    for (std::size_t i = 0; i < na; i++) if (sa[i] != sb[i]) same = 0;
    QWC_TRUE(same, "identical legal set at normal vs u16-max counters");

    /* 100/150 halfmoves (rule-50 draw thresholds) do not affect movegen. */
    Position c, d;
    QWC_TRUE(fen_load(&c, fens[k], &why) == 1, "load C");
    QWC_TRUE(fen_load(&d, fens[k], &why) == 1, "load D");
    c.halfmove = 100; c.fullmove = 1;  pos_rebuild(&c);
    d.halfmove = 150; d.fullmove = 1;  pos_rebuild(&d);
    Move oc[MGL_CAP], od[MGL_CAP]; std::size_t nc = 0, nd = 0;
    QWC_TRUE(movegen_legal_noncastling(c, std::span<Move>(oc, MGL_CAP), nc), "gen C (hm 100)");
    QWC_TRUE(movegen_legal_noncastling(d, std::span<Move>(od, MGL_CAP), nd), "gen D (hm 150)");
    QWC_EQ_I((int)nc, (int)na, "100-halfmove set == ordinary set");
    QWC_EQ_I((int)nd, (int)na, "150-halfmove set == ordinary set (draw adjudication is not movegen)");
    fail |= qwc_end();
  }

  /* an EP board: halfmove stays 0 (a valid recorded EP requires it); only the
   * fullmove is pushed to its maximum. The legal set is unchanged. */
  qwc_begin("mgl.counters.ep.maxfullmove");
  {
    const char *fen = "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20";   /* ep_1 */
    Position a, b; const char *why = nullptr;
    QWC_TRUE(fen_load(&a, fen, &why) == 1, "load A (hm 0)");
    QWC_TRUE(fen_load(&b, fen, &why) == 1, "load B (hm 0, fm max)");
    a.halfmove = 0; a.fullmove = 1;     pos_rebuild(&a);
    b.halfmove = 0; b.fullmove = 65535; pos_rebuild(&b);   /* EP requires hm 0 */
    Move oa[MGL_CAP], ob[MGL_CAP]; std::size_t na = 0, nb = 0;
    QWC_TRUE(movegen_legal_noncastling(a, std::span<Move>(oa, MGL_CAP), na), "gen A");
    QWC_TRUE(movegen_legal_noncastling(b, std::span<Move>(ob, MGL_CAP), nb), "gen B");
    QWC_EQ_I((int)na, (int)nb, "same count at fm 1 vs fm max");
    Move sa[MGL_CAP], sb[MGL_CAP];
    std::memcpy(sa, oa, na * sizeof(Move)); std::memcpy(sb, ob, nb * sizeof(Move));
    sort_moves(sa, na); sort_moves(sb, nb);
    int same = 1;
    for (std::size_t i = 0; i < na; i++) if (sa[i] != sb[i]) same = 0;
    QWC_TRUE(same, "identical legal set at fm 1 vs fm max");
  }
  fail |= qwc_end();

  /* the overflow guard is intact: an actual game make() at 65535 halfmove is
   * rejected (it is not the legal generator's job to make that move). */
  qwc_begin("mgl.counters.make_rejects_overflow");
  {
    Position a; const char *why = nullptr;
    QWC_TRUE(fen_load(&a, "7k/8/8/8/8/3p4/3R4/K7 w - - 0 1", &why) == 1, "load");
    a.halfmove = 65535; a.fullmove = 1; pos_rebuild(&a);
    StateInfo st{};
    Move quiet = make_move(square_of(0, 0), square_of(0, 1), 0u, 0u);   /* a1a2 (a quiet move -> hm+1) */
    QWC_EQ_I(pos_make_move(&a, &st, quiet), 0, "make() at 65535 halfmove is rejected (no overflow)");
  }
  fail |= qwc_end();
  return fail;
}

/* ---- integration round-trip -------------------------------------------------- */
/* For every emitted move on the ordinary-counter corpus: an ACTUAL make/unmake
 * succeeds, the child is internally consistent (pos_validate) and matches a cold
 * rebuild (caches + key), and the unmake restores the full logical state + history.
 * The cold rebuild is a test oracle here, not production filtering code. A legal
 * child is a valid Position, so (unlike the pseudo layer) pos_validate is asserted. */
static int t_integration() {
  int fail = 0;
  for (int k = 0; k < NB_FIXTURES; k++) {
    char tag[48]; std::snprintf(tag, sizeof tag, "mgl.integ.%d", k);
    qwc_begin(tag);
    Position pos; const char *why = nullptr; const char *w = nullptr;
    QWC_TRUE(fen_load(&pos, fixtures[k].fen, &why) == 1, "load");
    Move out[MGL_CAP]; std::size_t n = 0;
    QWC_TRUE(movegen_legal_noncastling(pos, std::span<Move>(out, MGL_CAP), n), "gen");
    Position base = pos;   /* carries the (nullptr) history pointer of a fresh FEN load */
    for (std::size_t i = 0; i < n; i++) {
      Position before = pos;
      StateInfo st{};
      QWC_TRUE(pos_make_move(&pos, &st, out[i]) == 1, "actual make succeeds");
      QWC_TRUE(derived_match_rebuild(&pos, &w), "child caches/key == cold rebuild");
      QWC_TRUE(pos_validate(&pos, &w), "child is a consistent Position");
      pos_unmake_move(&pos, &st);
      QWC_TRUE(pos_equal_full(&pos, &before), "unmake restores all logical state + history");
    }
    QWC_TRUE(pos_equal_full(&pos, &base), "board returned to the loaded state (history incl.)");
    fail |= qwc_end();
  }
  return fail;
}

int main() {
  int fail = 0;
  fail |= t_fixtures();
  fail |= t_capacity();
  fail |= t_preservation();
  fail |= t_counters();
  fail |= t_integration();
  std::printf("\nmovegen legal (non-castling): %s\n", fail ? "FAIL" : "all groups PASS");
  return fail ? 1 : 0;
}
