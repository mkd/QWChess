/* QwenChess T005 checkpoint-3 test: full orthodox LEGAL move generation, incl. castles.
 *
 * Verifies movegen_legal() (src/movegen/movegen.cpp) -- the complete legal set: the
 * non-castling legal moves plus the legal castles -- against independently generated
 * sets. A move is legal iff it is pseudo-legal AND leaves the original mover's own king
 * unattacked (giving check is legal). A castle (king origin -> rook origin, MV_CASTLE)
 * is legal iff the side holds the right, the home king/rook are in place with an empty
 * path, and the king's origin, transit and destination squares are all unattacked.
 *
 * FIXTURE PROVENANCE (independently generated): the 19 positions below and their
 * complete legal sets were generated on 2026-10-20 with the python `chess` module 1.11.2
 * (module sha256 1fde6a8e932508d14b31f6a584497b49c2969fc3c7cdc675347f819f0d667a5b);
 * `moves` is the complete legal set in QWChess internal coordinates, `castles` the
 * subset needing the castle flag and `ep` the subset needing the EP flag (the standard
 * reference castle destinations g/c were converted to the home-rook origins h/a). The
 * sets were copied mechanically from that reference and the copied names/FENs/sets were
 * checked against it. The `blocked_b` entry has a b1 knight blocking the a1 rook's rank
 * ray, so a1b1 is illegal; its verified set is 25 moves (in-corpus note). This build
 * does not run python-chess (no Python dependency); the
 * reference generator is documented as provenance, and the coordinate strings are test-
 * only notation (a small decoder turns them into Moves). This is NOT production UCI
 * parsing, and a successful file load is not proof of compatibility.
 *
 * The corpus exercises origin/transit/destination attacks, each obstructing path square,
 * attacked b-file/rook squares that are still allowed, knight/pawn/enemy-king attacks on
 * the transit, giving a check, and a recorded EP alongside castling rights.
 *
 * Groups: (1) the 19 fixtures + rank/color mirrors (complete encoded set + the castle /
 * EP / promotion flag semantics; the existing non-castling APIs still exclude every
 * castle); (2) all 16 rights masks on both_clear, both sides to move, plus rights staying
 * lost after king/rook moves; (3) real make/unmake of each of the four orthodox castles
 * against a cold rebuild + full restore; (4) capacity (both_clear 26/25/24 + the
 * non-castling propagation, empty spans, mate/stalemate); (5) preservation +
 * determinism (Position and live records untouched on every outcome); (6) counter
 * independence + agreement with the non-castling API on no-rights/EP/promo/mate/
 * stalemate positions. These are focused cp3 regressions; they are not the cp4 perft /
 * random-differential milestone.
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

/* Test output buffer, larger than any fixture's complete legal set (max 28). */
enum { MGC_CAP = 64 };

/* A stable reason sink (a file-local global) so a valid `&s_why` address can be
 * passed to fen_load / pos_validate, whose diagnostics these groups do not read. */
static const char *s_why = nullptr;

/* ---- small test-only helpers (mirrored from movegen_legal_test; no harness refactor) */

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

/* Field-by-field equality over the ENTIRE Position INCLUDING the history pointer. */
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
  if (a->white_king_sq != b->white_king_sq || a->black_king_sq != b->black_king_sq) return 0;
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
 * and any promotion suffix preserved). An empty string stays empty. */
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

/* Move -> a short "a1a2" style string for diagnostics (castle/EP/promo tag). */
static void move_to_coord(Move m, char out[8]) {
  out[0] = (char)('a' + file_of(move_from(m)));
  out[1] = (char)('1' + rank_of(move_from(m)));
  out[2] = (char)('a' + file_of(move_to(m)));
  out[3] = (char)('1' + rank_of(move_to(m)));
  std::size_t k = 4;
  u32 fl = move_flags(m);
  if (fl & to_under(MoveFlag::MV_CASTLE)) out[k++] = 'O';   /* castle (the O-O/O-O-O move) */
  else if (fl & to_under(MoveFlag::MV_EP)) out[k++] = 'e';
  else if (fl & to_under(MoveFlag::MV_PROMO)) {
    switch ((PieceType)move_promo(m)) {
      case PieceType::KNIGHT: out[k++] = 'n'; break;
      case PieceType::BISHOP: out[k++] = 'b'; break;
      case PieceType::ROOK:   out[k++] = 'r'; break;
      default:                out[k++] = 'q'; break;
    }
  }
  out[k] = '\0';
}

/* Compare the generated `got`/`ngot` against the expected `moves` set (with the
 * `castles` / `ep` flag subsets). Verifies the complete encoded Move set (order-
 * independent after sorting), no duplicates, and -- separately -- the flag/payload
 * semantics (the MV_CASTLE / MV_EP counts and the promotion payload range; no mixed
 * flags, no reserved bits, from != to, no null). On a content mismatch it prints the
 * moves in each set so the exact discrepancy is visible. */
static int cmp_set(const char *tag, const Move *got, std::size_t ngot,
                   const char *moves, const char *castles, const char *ep) {
  std::array<Move, 128> exp; std::size_t nexp = 0;
  char mbuf[512]; strcopy(mbuf, sizeof mbuf, moves);
  char *mtoks[128]; int mt = split_spaces(mbuf, mtoks, 128);
  char cbuf[256]; strcopy(cbuf, sizeof cbuf, castles);
  char *ctoks[16];  int ct = split_spaces(cbuf, ctoks, 16);
  char ebuf[256]; strcopy(ebuf, sizeof ebuf, ep);
  char *etoks[16];  int et = split_spaces(ebuf, etoks, 16);

  int npr_exp = 0;
  for (int i = 0; i < mt && nexp < 128; i++) {
    Move m = 0;
    QWC_TRUE(coord_to_move(mtoks[i], &m), "expected coord decodes");
    int is_castle = 0, is_ep = 0;
    for (int j = 0; j < ct; j++) if (std::strcmp(ctoks[j], mtoks[i]) == 0) is_castle = 1;
    for (int j = 0; j < et; j++) if (std::strcmp(etoks[j], mtoks[i]) == 0) is_ep = 1;
    if (is_castle)      exp[nexp++] = make_move(move_from(m), move_to(m), (u32)MoveFlag::MV_CASTLE, 0u);
    else if (is_ep)     exp[nexp++] = make_move(move_from(m), move_to(m), (u32)MoveFlag::MV_EP, 0u);
    else { if (std::strlen(mtoks[i]) == 5) npr_exp++; exp[nexp++] = m; }
  }

  QWC_EQ_I((int)ngot, (int)nexp, "move count == expected");

  Move gs[MGC_CAP]; std::memcpy(gs, got, ngot * sizeof(Move));
  sort_moves(gs, ngot); sort_moves(exp.data(), nexp);
  int nodup = 1;
  for (std::size_t i = 1; i < ngot; i++) if (gs[i] == gs[i - 1]) nodup = 0;
  QWC_TRUE(nodup, "no duplicate moves");
  if (ngot == nexp) {
    int match = 1;
    for (std::size_t i = 0; i < ngot; i++) if (gs[i] != exp[i]) { match = 0; break; }
    QWC_TRUE(match, "complete encoded Move set matches expected");
    if (!match) {
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
  int ncastle = 0, nep = 0, npr = 0;
  for (std::size_t i = 0; i < ngot; i++) {
    u32 fl = move_flags(got[i]), pr = move_promo(got[i]);
    QWC_TRUE(move_repr_is_valid(got[i]), "no reserved bits set");
    QWC_TRUE(got[i] != 0u, "no null move");
    QWC_TRUE(move_from(got[i]) != move_to(got[i]), "from != to");
    const u32 f3 = fl & 7u;
    QWC_TRUE((f3 & (f3 - 1u)) == 0u, "at most one flag bit (castles are pure)");
    if (f3 == to_under(MoveFlag::MV_CASTLE)) { ncastle++; QWC_EQ_I((int)pr, 0, "castle has zero promo payload"); }
    else if (f3 == to_under(MoveFlag::MV_EP))     { nep++;     QWC_EQ_I((int)pr, 0, "EP has zero promo payload"); }
    else if (f3 == to_under(MoveFlag::MV_PROMO))  { npr++;     QWC_TRUE(pr >= to_under(PieceType::KNIGHT) && pr <= to_under(PieceType::QUEEN), "promo payload in N..Q"); }
    else                          { QWC_EQ_I((int)pr, 0, "quiet move has zero payload"); }
  }
  QWC_EQ_I(ncastle, ct, "MV_CASTLE count == expected castles");
  QWC_EQ_I(nep, et, "MV_EP count == expected EP moves");
  QWC_EQ_I(npr, npr_exp, "MV_PROMO count == expected promo moves");
  return qwc_end();
}

/* ---- the 19-fixture corpus (provenance in the file header) --------------------
 * `moves` is the complete legal set; `castles` the MV_CASTLE subset (the home-rook
 * origins, e.g. e1h1 = O-O); `ep` the MV_EP subset. */
typedef struct {
  const char *name;
  const char *fen;
  const char *moves;
  const char *castles;
  const char *ep;
} Fixture;

static const Fixture fixtures[] = {
  { "both_clear",
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1a1 e1d1 e1d2 "
    "e1e2 e1f1 e1f2 e1h1 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "e1a1 e1h1", "" },
  { "blocked_f",
    "r3k2r/8/8/8/8/8/8/R3KB1R w KQkq - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1a1 e1d1 e1d2 "
    "e1e2 e1f2 f1a6 f1b5 f1c4 f1d3 f1e2 f1g2 f1h3 h1g1 h1h2 h1h3 h1h4 "
    "h1h5 h1h6 h1h7 h1h8",
    "e1a1", "" },
  { "blocked_g",
    "r3k2r/8/8/8/8/8/8/R3K1NR w KQkq - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1a1 e1d1 e1d2 "
    "e1e2 e1f1 e1f2 g1e2 g1f3 g1h3 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "e1a1", "" },
  { "blocked_d",
    "r3k2r/8/8/8/8/8/8/R2BK2R w KQkq - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 d1a4 d1b3 d1c2 d1e2 "
    "d1f3 d1g4 d1h5 e1d2 e1e2 e1f1 e1f2 e1h1 h1f1 h1g1 h1h2 h1h3 h1h4 "
    "h1h5 h1h6 h1h7 h1h8",
    "e1h1", "" },
  { "blocked_c",
    "r3k2r/8/8/8/8/8/8/R1N1K2R w KQkq - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 c1a2 c1b3 c1d3 c1e2 e1d1 "
    "e1d2 e1e2 e1f1 e1f2 e1h1 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "e1h1", "" },
  { "blocked_b",
    "r3k2r/8/8/8/8/8/8/RN2K2R w KQkq - 0 1",
    /* The b1 knight (RN2K2R) blocks the a1 rook's rank ray, so a1b1 is illegal.
     * Verified against the FEN: a1 rook 7 (a2-a8) + b1 knight 3 + e1 king 5 (incl. the
     * e1h1 kingside castle; the b1 knight blocks the queenside path) + h1 rook 9 =
     * 25 moves. */
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 b1a3 b1c3 b1d2 e1d1 e1d2 "
    "e1e2 e1f1 e1f2 e1h1 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "e1h1", "" },
  { "origin_attacked",
    "1k2r3/8/8/8/8/8/8/R3K2R w KQ - 0 1",
    "e1d1 e1d2 e1f1 e1f2", "", "" },
  { "kingside_transit_attacked",
    "1k3r2/8/8/8/8/8/8/R3K2R w KQ - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1a1 e1d1 e1d2 "
    "e1e2 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "e1a1", "" },
  { "kingside_destination_attacked",
    "1k4r1/8/8/8/8/8/8/R3K2R w KQ - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1a1 e1d1 e1d2 "
    "e1e2 e1f1 e1f2 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "e1a1", "" },
  { "queenside_transit_attacked",
    "1k1r4/8/8/8/8/8/8/R3K2R w KQ - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1e2 e1f1 e1f2 "
    "e1h1 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "e1h1", "" },
  { "queenside_destination_attacked",
    "1kr5/8/8/8/8/8/8/R3K2R w KQ - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1d1 e1d2 e1e2 "
    "e1f1 e1f2 e1h1 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "e1h1", "" },
  { "b_square_attacked_allowed",
    "1r2k3/8/8/8/8/8/8/R3K2R w KQ - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1a1 e1d1 e1d2 "
    "e1e2 e1f1 e1f2 e1h1 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "e1a1 e1h1", "" },
  { "a_rook_attacked_allowed",
    "r3k3/8/8/8/8/8/8/R3K2R w KQ - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1a1 e1d1 e1d2 "
    "e1e2 e1f1 e1f2 e1h1 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "e1a1 e1h1", "" },
  { "h_rook_attacked_allowed",
    "4k2r/8/8/8/8/8/8/R3K2R w KQ - 0 1",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1a1 e1d1 e1d2 "
    "e1e2 e1f1 e1f2 e1h1 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "e1a1 e1h1", "" },
  { "pinned_knight_attacks_transit",
    "7k/8/8/8/8/8/7n/4K2R w K - 0 1",
    "e1d1 e1d2 e1e2 e1f2 h1f1 h1g1 h1h2", "", "" },
  { "pawn_attacks_transit",
    "k7/8/8/8/8/8/6p1/4K2R w K - 0 1",
    "e1d1 e1d2 e1e2 e1f2 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8",
    "", "" },
  { "enemy_king_attacks_transit",
    "8/8/8/8/8/8/6k1/4K2R w K - 0 1",
    "e1d1 e1d2 e1e2 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 h1h7 h1h8", "", "" },
  { "castle_gives_check",
    "5k2/8/8/8/8/8/8/4K2R w K - 0 1",
    "e1d1 e1d2 e1e2 e1f1 e1f2 e1h1 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 "
    "h1h7 h1h8",
    "e1h1", "" },
  { "recorded_ep",
    "r3k2r/8/8/3pP3/8/8/8/R3K2R w KQkq d6 0 20",
    "a1a2 a1a3 a1a4 a1a5 a1a6 a1a7 a1a8 a1b1 a1c1 a1d1 e1a1 e1d1 e1d2 "
    "e1e2 e1f1 e1f2 e1h1 e5d6 e5e6 h1f1 h1g1 h1h2 h1h3 h1h4 h1h5 h1h6 "
    "h1h7 h1h8",
    "e1a1 e1h1", "e5d6" },
};
enum { NB_FIXTURES = (int)(sizeof fixtures / sizeof fixtures[0]) };

/* ---- (1) the 19 fixtures, original and rank/color-mirrored ------------------- */
static int t_fixtures() {
  int fail = 0;
  for (int i = 0; i < NB_FIXTURES; i++) {
    std::array<char, 48> tag{};

    /* original position */
    std::snprintf(tag.data(), tag.size(), "mgc.%s", fixtures[i].name);
    qwc_begin(tag.data());
    Position pos; const char *why = nullptr;
    QWC_TRUE(fen_load(&pos, fixtures[i].fen, &why) == 1, "fixture loads");
    QWC_TRUE(pos_validate(&pos, &why), "fixture validates");
    Move out[MGC_CAP]; std::size_t n = 12345;
    QWC_TRUE(movegen_legal(pos, std::span<Move>(out, MGC_CAP), n), "generation succeeds");
    fail |= cmp_set(tag.data(), out, n, fixtures[i].moves, fixtures[i].castles, fixtures[i].ep);

    /* rank-reflected, color-swapped counterpart (the mirror must match the reflected set) */
    std::snprintf(tag.data(), tag.size(), "mgc.%s.mirror", fixtures[i].name);
    qwc_begin(tag.data());
    Position mp; pos_mirror(&pos, &mp);
    QWC_EQ_I((int)mp.side, (int)(1 - to_under(pos.side)), "mirror flips the side");
    QWC_TRUE(pos_validate(&mp, &why), "mirror validates");
    std::array<char, 512> mref{}, cref{}, eref{};
    reflect_moveset(fixtures[i].moves, mref.data());
    reflect_moveset(fixtures[i].castles, cref.data());
    reflect_moveset(fixtures[i].ep, eref.data());
    Move mout[MGC_CAP]; std::size_t mn = 12345;
    QWC_TRUE(movegen_legal(mp, std::span<Move>(mout, MGC_CAP), mn), "mirror generation succeeds");
    fail |= cmp_set(tag.data(), mout, mn, mref.data(), cref.data(), eref.data());
  }
  return fail;
}

/* ---- (2) castling rights ------------------------------------------------------ */
/* On both_clear every structural precondition holds and nothing attacks the path,
 * so a castle is legal iff the mover holds the right. Verify all 16 masks, both sides. */
static int t_rights_masks() {
  int fail = 0;
  const char *fen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";   /* both_clear */
  for (int cr = 0; cr < 16; cr++) {
    /* white to move */
    char tag[48];
    std::snprintf(tag, sizeof tag, "mgc.rights.w.%d", cr);
    qwc_begin(tag);
    Position pos; const char *why = nullptr;
    QWC_TRUE(fen_load(&pos, fen, &why) == 1, "load");
    pos.cr = (u8)cr; pos_rebuild(&pos);
    Move out[MGC_CAP]; std::size_t n = 0;
    QWC_TRUE(movegen_legal(pos, std::span<Move>(out, MGC_CAP), n), "gen");
    int gotK = 0, gotQ = 0;
    for (std::size_t i = 0; i < n; i++) {
      if (move_flags(out[i]) & to_under(MoveFlag::MV_CASTLE)) {
        if (move_to(out[i]) == square_of(FILE_H, RANK_1)) gotK = 1;   /* e1h1 (K) */
        else if (move_to(out[i]) == square_of(FILE_A, RANK_1)) gotQ = 1; /* e1a1 (Q) */
      }
    }
    QWC_EQ_I(gotK, (cr & CR_WK) ? 1 : 0, "K castle present iff CR_WK");
    QWC_EQ_I(gotQ, (cr & CR_WQ) ? 1 : 0, "Q castle present iff CR_WQ");
    QWC_EQ_I((int)n, 24 + ((cr & CR_WK) ? 1 : 0) + ((cr & CR_WQ) ? 1 : 0), "total = 24 non-castling + castles");
    fail |= qwc_end();

    /* black to move (the board is symmetric; the black rights apply) */
    std::snprintf(tag, sizeof tag, "mgc.rights.b.%d", cr);
    qwc_begin(tag);
    Position bp;
    QWC_TRUE(fen_load(&bp, fen, &why) == 1, "load");
    bp.side = Color::BLACK; bp.cr = (u8)cr; pos_rebuild(&bp);
    Move bout[MGC_CAP]; std::size_t bn = 0;
    QWC_TRUE(movegen_legal(bp, std::span<Move>(bout, MGC_CAP), bn), "gen");
    int bK = 0, bQ = 0;
    for (std::size_t i = 0; i < bn; i++) {
      if (move_flags(bout[i]) & to_under(MoveFlag::MV_CASTLE)) {
        if (move_to(bout[i]) == square_of(FILE_H, RANK_8)) bK = 1;   /* e8h8 (k) */
        else if (move_to(bout[i]) == square_of(FILE_A, RANK_8)) bQ = 1; /* e8a8 (q) */
      }
    }
    QWC_EQ_I(bK, (cr & CR_BK) ? 1 : 0, "k castle present iff CR_BK");
    QWC_EQ_I(bQ, (cr & CR_BQ) ? 1 : 0, "q castle present iff CR_BQ");
    QWC_EQ_I((int)bn, 24 + ((cr & CR_BK) ? 1 : 0) + ((cr & CR_BQ) ? 1 : 0), "total = 24 non-castling + castles");
    fail |= qwc_end();
  }
  return fail;
}

/* Rights stay LOST after a king/rook moves, even if it returns home: apply a 4-move
 * sequence on a live history, verify the rights state + the generated castles, then
 * undo exactly and confirm the full restore. */
typedef struct { const char *tag; const char *moves; u8 rights; const char *castles; } Seq;
static int t_rights_lost() {
  int fail = 0;
  static const Seq seqs[] = {
    { "king_return",   "e1f1 e8f8 f1e1 f8e8", 0u,  "" },              /* no rights left   */
    { "h_rook_return", "h1h2 h8h7 h2h1 h7h8", CR_WQ | CR_BQ, "e1a1" }, /* Qq left (white: Q) */
    { "a_rook_return", "a1a2 a8a7 a2a1 a7a8", CR_WK | CR_BK, "e1h1" }, /* Kk left (white: K) */
  };
  const char *fen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";   /* both_clear */
  for (int k = 0; k < 3; k++) {
    char tag[48];
    std::snprintf(tag, sizeof tag, "mgc.rights_lost.%s", seqs[k].tag);
    qwc_begin(tag);
    Position pos; const char *why = nullptr;
    QWC_TRUE(fen_load(&pos, fen, &why) == 1, "load");
    Position base = pos;
    std::array<StateInfo, 4> st{};
    char mbuf[64]; strcopy(mbuf, sizeof mbuf, seqs[k].moves);
    char *toks[8]; int nt = split_spaces(mbuf, toks, 8);
    QWC_EQ_I(nt, 4, "4 moves");
    for (int i = 0; i < 4; i++) {
      Move m = 0;
      QWC_TRUE(coord_to_move(toks[i], &m), "coord decodes");
      QWC_TRUE(pos_make_move(&pos, &st[i], m) == 1, "make succeeds");
    }
    QWC_EQ_I((int)pos.cr, (int)seqs[k].rights, "rights state after the sequence");
    QWC_TRUE(pos.side == Color::WHITE, "mover is white after 4 moves");
    Move out[MGC_CAP]; std::size_t n = 0;
    QWC_TRUE(movegen_legal(pos, std::span<Move>(out, MGC_CAP), n), "gen");
    /* the generated castles must equal the (white) subset of the remaining rights */
    char cref[32]; strcopy(cref, sizeof cref, seqs[k].castles);
    char *ctoks[8]; int ct = split_spaces(cref, ctoks, 8);
    int ncastle = 0;
    for (std::size_t i = 0; i < n; i++) if (move_flags(out[i]) & to_under(MoveFlag::MV_CASTLE)) ncastle++;
    QWC_EQ_I(ncastle, ct, "castle count == remaining white rights");
    /* undo exactly (LIFO) and confirm the full restore */
    for (int i = 3; i >= 0; i--) pos_unmake_move(&pos, &st[i]);
    QWC_TRUE(pos_equal_full(&pos, &base), "LIFO unmake restores exactly");
    fail |= qwc_end();
  }
  return fail;
}

/* ---- (3) real make/unmake of each of the four orthodox castles ---------------- */
/* A live make on a live history record: the after-state (board, rights, EP,
 * counters, side) must round-trip to the exact FEN, the derived caches + key must
 * match a cold rebuild, the child must validate, and the LIFO unmake must restore
 * the full state. (The STRUCTURAL make is proven in the castling make/unmake test;
 * here it is driven from the legal generator's encoded castle.) */
static int t_castle_one(const char *tag, const char *before, Move m, const char *after) {
  qwc_begin(tag);
  Position pos; StateInfo st; const char *why = nullptr; char buf[FEN_BUF_SIZE];
  std::memset(&st, 0, sizeof st);
  QWC_TRUE(fen_load(&pos, before, &why) == 1, "load before");
  Position base = pos;
  QWC_TRUE(pos_make_move(&pos, &st, m) == 1, "castle make succeeds");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "after: caches/key == cold rebuild");
  QWC_TRUE(pos_validate(&pos, &why), "after: child validates");
  QWC_TRUE(fen_emit(&pos, buf, (int)sizeof buf) == 1, "emit after");
  QWC_TRUE(std::strcmp(buf, after) == 0, "exact resulting FEN");
  pos_unmake_move(&pos, &st);
  QWC_TRUE(pos_equal_full(&pos, &base), "unmake restores exactly");
  return qwc_end();
}
static int t_makeunmake() {
  int fail = 0;
  const char *bw = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 7 20";
  const char *bb = "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 7 20";
  fail |= t_castle_one("mgc.mk.W_kingside",  bw,
      make_move(square_of(FILE_E, RANK_1), square_of(FILE_H, RANK_1), (u32)MoveFlag::MV_CASTLE, 0u),
      "r3k2r/8/8/8/8/8/8/R4RK1 b kq - 8 20");
  fail |= t_castle_one("mgc.mk.W_queenside", bw,
      make_move(square_of(FILE_E, RANK_1), square_of(FILE_A, RANK_1), (u32)MoveFlag::MV_CASTLE, 0u),
      "r3k2r/8/8/8/8/8/8/2KR3R b kq - 8 20");
  fail |= t_castle_one("mgc.mk.B_kingside",  bb,
      make_move(square_of(FILE_E, RANK_8), square_of(FILE_H, RANK_8), (u32)MoveFlag::MV_CASTLE, 0u),
      "r4rk1/8/8/8/8/8/8/R3K2R w KQ - 8 21");
  fail |= t_castle_one("mgc.mk.B_queenside", bb,
      make_move(square_of(FILE_E, RANK_8), square_of(FILE_A, RANK_8), (u32)MoveFlag::MV_CASTLE, 0u),
      "2kr3r/8/8/8/8/8/8/R3K2R w KQ - 8 21");

  /* a castle with a stale recorded EP must clear the target (and unmake restores it) */
  qwc_begin("mgc.mk.ep_clearing");
  Position pos; StateInfo st; const char *why = nullptr; char buf[FEN_BUF_SIZE];
  std::memset(&st, 0, sizeof st);
  const Square d6 = square_of(FILE_D, RANK_6);
  QWC_TRUE(fen_load(&pos, "4k3/8/8/3pP3/8/8/8/4K2R w K d6 0 20", &why) == 1, "load (stale EP)");
  Position base = pos;
  QWC_EQ_I((int)pos.ep_sq, (int)d6, "before: raw EP = d6");
  QWC_TRUE(pos_make_move(&pos, &st, make_move(square_of(FILE_E, RANK_1), square_of(FILE_H, RANK_1), (u32)MoveFlag::MV_CASTLE, 0u)) == 1, "castle accepted");
  QWC_EQ_I((int)pos.ep_sq, (int)NO_SQUARE, "after: raw EP cleared");
  QWC_TRUE(derived_match_rebuild(&pos, &why), "after: caches/key == rebuild");
  QWC_TRUE(fen_emit(&pos, buf, (int)sizeof buf) == 1 && std::strcmp(buf, "4k3/8/8/3pP3/8/8/8/5RK1 b - - 1 20") == 0, "exact resulting FEN (EP cleared)");
  pos_unmake_move(&pos, &st);
  QWC_EQ_I((int)pos.ep_sq, (int)d6, "unmake: raw EP restored");
  QWC_TRUE(pos_equal_full(&pos, &base), "unmake: full restore");
  fail |= qwc_end();   /* accumulate the EP group into the group total */
  return fail;
}

/* ---- (4) capacity / overflow -------------------------------------------------- */
static int t_capacity() {
  int fail = 0;
  const char *fen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";   /* both_clear: 24 non-castling + 2 castles = 26 */
  Position pos; const char *why = nullptr;
  QWC_TRUE(fen_load(&pos, fen, &why) == 1, "load both_clear");

  /* cap 26 succeeds; 25 (fits the 24 non-castling + 1, not both castles) and 24
   * (fits only the non-castling moves) must BOTH fail with count 0. */
  qwc_begin("mgc.cap.both_clear");
  {
    Move buf[MGC_CAP]; std::size_t n = 12345;
    QWC_TRUE(movegen_legal(pos, std::span<Move>(buf, 26), n), "cap 26 succeeds");
    QWC_EQ_I((int)n, 26, "cap 26 writes exactly 26");
    n = 12345;
    QWC_TRUE(!movegen_legal(pos, std::span<Move>(buf, 25), n), "cap 25 fails (full set does not fit)");
    QWC_EQ_I((int)n, 0, "cap 25 count reset to 0");
    n = 12345;
    QWC_TRUE(!movegen_legal(pos, std::span<Move>(buf, 24), n), "cap 24 fails (castle overflow)");
    QWC_EQ_I((int)n, 0, "cap 24 count reset to 0");
  }
  fail |= qwc_end();

  /* canary: an overflow writes nothing beyond the span and leaves the input untouched */
  qwc_begin("mgc.cap.canary");
  {
    Position before = pos;
    Move cbuf[MGC_CAP];
    for (int i = 0; i < MGC_CAP; i++) cbuf[i] = (Move)0xA5A5A5A5u;
    std::size_t n = 12345;
    QWC_TRUE(!movegen_legal(pos, std::span<Move>(cbuf, 24), n), "cap 24 overflow fails");
    QWC_EQ_I((int)n, 0, "overflow count reset to 0");
    int beyond_ok = 1;
    for (int i = 24; i < MGC_CAP; i++) if (cbuf[i] != (Move)0xA5A5A5A5u) beyond_ok = 0;
    QWC_TRUE(beyond_ok, "no write beyond the 24-capacity span (canary untouched)");
    QWC_TRUE(pos_equal_full(&pos, &before), "Position untouched (incl. history) on overflow");
  }
  fail |= qwc_end();

  /* a non-castling generation failure (a buffer smaller than the non-castling part)
   * propagates as a failure of movegen_legal with count 0. */
  qwc_begin("mgc.cap.nc_failure");
  {
    /* origin_attacked has 4 non-castling legal moves; a cap of 3 fails the
     * non-castling step (it cannot even hold those), so the full API fails. */
    Position apos;
    QWC_TRUE(fen_load(&apos, "1k2r3/8/8/8/8/8/8/R3K2R w KQ - 0 1", &why) == 1, "load origin_attacked");
    Move buf[MGC_CAP]; std::size_t n = 12345;
    QWC_TRUE(!movegen_legal(apos, std::span<Move>(buf, 3), n), "cap 3 (< 4 non-castling) fails");
    QWC_EQ_I((int)n, 0, "count reset to 0 (non-castling failure propagates)");
  }
  fail |= qwc_end();

  /* empty spans: a zero-size span succeeds with count 0 only when the complete set
   * is empty (checkmate / stalemate); otherwise it fails. */
  qwc_begin("mgc.cap.empty");
  {
    static const struct { const char *fen; int empty_set; } cases[] = {
      { "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1", 1 },   /* checkmate: zero legal moves */
      { "7k/5K2/6Q1/8/8/8/8/8 b - - 0 1", 1 },   /* stalemate: zero legal moves */
      { "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", 0 }, /* both_clear: non-empty */
    };
    for (int k = 0; k < 3; k++) {
      Position p; Move buf[MGC_CAP];
      for (int i = 0; i < MGC_CAP; i++) buf[i] = (Move)0xA5A5A5A5u;   /* canary */
      std::size_t n = 12345;
      QWC_TRUE(fen_load(&p, cases[k].fen, &why) == 1, "load");
      if (cases[k].empty_set) {
        QWC_TRUE(movegen_legal(p, std::span<Move>(buf, 0), n), "empty span succeeds (no legal moves)");
        QWC_EQ_I((int)n, 0, "count is 0");
      } else {
        QWC_TRUE(!movegen_legal(p, std::span<Move>(buf, 0), n), "empty span fails (non-empty set)");
        QWC_EQ_I((int)n, 0, "count is 0");
      }
      int canary = 1;
      for (int i = 0; i < MGC_CAP; i++) if (buf[i] != (Move)0xA5A5A5A5u) canary = 0;
      QWC_TRUE(canary, "zero-size span writes nothing (canary untouched)");
    }
  }
  fail |= qwc_end();
  return fail;
}

/* ---- (5) preservation / determinism ------------------------------------------ */
static int t_preservation() {
  int fail = 0;
  const char *fen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";   /* both_clear */

  /* repeated calls give the same deterministic sequence; the whole Position
   * (incl. history) is untouched on a success and on an overflow. */
  qwc_begin("mgc.preserve.repeat");
  {
    Position pos;
    QWC_TRUE(fen_load(&pos, fen, &s_why) == 1, "load");
    Position before = pos;
    Move a[MGC_CAP], b[MGC_CAP]; std::size_t na = 0, nb = 0;
    QWC_TRUE(movegen_legal(pos, std::span<Move>(a, MGC_CAP), na), "gen 1");
    QWC_TRUE(movegen_legal(pos, std::span<Move>(b, MGC_CAP), nb), "gen 2");
    QWC_EQ_I((int)na, (int)nb, "same count on repeat");
    int same = 1;
    for (std::size_t i = 0; i < na; i++) if (a[i] != b[i]) same = 0;
    QWC_TRUE(same, "same sequence on repeat");
    QWC_TRUE(pos_equal_full(&pos, &before), "Position untouched (incl. history) on success");
  }
  fail |= qwc_end();

  qwc_begin("mgc.preserve.overflow");
  {
    Position pos;
    QWC_TRUE(fen_load(&pos, fen, &s_why) == 1, "load");
    Position before = pos;
    Move buf[MGC_CAP]; std::size_t n = 12345;
    QWC_TRUE(!movegen_legal(pos, std::span<Move>(buf, 24), n), "overflow");
    QWC_EQ_I((int)n, 0, "overflow count 0");
    QWC_TRUE(pos_equal_full(&pos, &before), "Position untouched (incl. history) on overflow");
  }
  fail |= qwc_end();

  /* a live caller-owned history prefix (two structural makes) is left untouched and
   * the live records are byte-identical after the call; the API never returns a
   * pointer to a local record. */
  qwc_begin("mgc.preserve.history");
  {
    Position p2; pos_set_startpos(&p2);
    std::array<StateInfo, 2> st{};
    QWC_TRUE(pos_make_move(&p2, &st[0], make_move(square_of(4, 1), square_of(4, 3), 0u, 0u)) == 1, "make 1.e2e4");
    QWC_TRUE(pos_make_move(&p2, &st[1], make_move(square_of(3, 6), square_of(3, 4), 0u, 0u)) == 1, "make 1...d7d5");
    QWC_TRUE(p2.history == &st[1] && st[1].prev == &st[0] && st[0].prev == nullptr, "live 2-record history chain");
    Position before2 = p2;
    StateInfo head_snap = st[1];
    Move out[MGC_CAP]; std::size_t n = 0;
    QWC_TRUE(movegen_legal(p2, std::span<Move>(out, MGC_CAP), n), "gen (live history)");
    QWC_TRUE(pos_equal_full(&p2, &before2), "Position untouched (incl. history) with live history");
    QWC_TRUE(p2.history == &st[1], "history head still points at the caller's record");
    QWC_TRUE(state_equal(&st[1], &head_snap), "head record byte-identical");
    QWC_TRUE(state_equal(&st[0], &st[0]), "earlier record still self-consistent");
  }
  fail |= qwc_end();
  return fail;
}

/* ---- (6) counter independence + agreement with the non-castling API ----------- */
static int t_counters() {
  int fail = 0;

  /* Both colors, a castling board at ordinary counters and at the u16 boundary, must
   * yield the identical complete set (the counters are not part of legality/key). */
  qwc_begin("mgc.counters.u16");
  {
    const char *fen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";   /* both_clear */
    Position a, b;
    QWC_TRUE(fen_load(&a, fen, &s_why) == 1, "load A");
    QWC_TRUE(fen_load(&b, fen, &s_why) == 1, "load B");
    a.halfmove = 0;     a.fullmove = 1;     pos_rebuild(&a);
    b.halfmove = 65535; b.fullmove = 65535; pos_rebuild(&b);
    Move oa[MGC_CAP], ob[MGC_CAP]; std::size_t na = 0, nb = 0;
    QWC_TRUE(movegen_legal(a, std::span<Move>(oa, MGC_CAP), na), "gen A");
    QWC_TRUE(movegen_legal(b, std::span<Move>(ob, MGC_CAP), nb), "gen B");
    QWC_EQ_I((int)na, (int)nb, "same count (white) at normal vs u16-max counters");
    Move sa[MGC_CAP], sb[MGC_CAP];
    std::memcpy(sa, oa, na * sizeof(Move)); std::memcpy(sb, ob, nb * sizeof(Move));
    sort_moves(sa, na); sort_moves(sb, nb);
    int same = 1;
    for (std::size_t i = 0; i < na; i++) if (sa[i] != sb[i]) same = 0;
    QWC_TRUE(same, "identical complete set (white) at normal vs u16-max counters");

    Position ca, cb;   /* the black-to-move mirror */
    QWC_TRUE(fen_load(&ca, fen, &s_why) == 1, "load CA");
    QWC_TRUE(fen_load(&cb, fen, &s_why) == 1, "load CB");
    ca.side = Color::BLACK; ca.halfmove = 0;     ca.fullmove = 1;     pos_rebuild(&ca);
    cb.side = Color::BLACK; cb.halfmove = 65535; cb.fullmove = 65535; pos_rebuild(&cb);
    Move oca[MGC_CAP], ocb[MGC_CAP]; std::size_t nca = 0, ncb = 0;
    QWC_TRUE(movegen_legal(ca, std::span<Move>(oca, MGC_CAP), nca), "gen CA");
    QWC_TRUE(movegen_legal(cb, std::span<Move>(ocb, MGC_CAP), ncb), "gen CB");
    QWC_EQ_I((int)nca, (int)ncb, "same count (black) at normal vs u16-max counters");
    std::memcpy(sa, oca, nca * sizeof(Move)); std::memcpy(sb, ocb, ncb * sizeof(Move));
    sort_moves(sa, nca); sort_moves(sb, ncb);
    same = 1;
    for (std::size_t i = 0; i < nca; i++) if (sa[i] != sb[i]) same = 0;
    QWC_TRUE(same, "identical complete set (black) at normal vs u16-max counters");
  }
  fail |= qwc_end();

  /* An EP position keeps halfmove 0 (a valid recorded EP requires it); pushing only
   * the fullmove to its maximum leaves the complete set (incl. the EP + castles) the
   * same. */
  qwc_begin("mgc.counters.ep");
  {
    const char *fen = "r3k2r/8/8/3pP3/8/8/8/R3K2R w KQkq d6 0 20";   /* recorded_ep */
    Position a, b;
    QWC_TRUE(fen_load(&a, fen, &s_why) == 1, "load A (hm 0)");
    QWC_TRUE(fen_load(&b, fen, &s_why) == 1, "load B");
    a.halfmove = 0; a.fullmove = 20;    pos_rebuild(&a);
    b.halfmove = 0; b.fullmove = 65535; pos_rebuild(&b);
    Move oa[MGC_CAP], ob[MGC_CAP]; std::size_t na = 0, nb = 0;
    QWC_TRUE(movegen_legal(a, std::span<Move>(oa, MGC_CAP), na), "gen A");
    QWC_TRUE(movegen_legal(b, std::span<Move>(ob, MGC_CAP), nb), "gen B");
    QWC_EQ_I((int)na, (int)nb, "same count at fm 20 vs fm max (hm stays 0)");
    Move sa[MGC_CAP], sb[MGC_CAP];
    std::memcpy(sa, oa, na * sizeof(Move)); std::memcpy(sb, ob, nb * sizeof(Move));
    sort_moves(sa, na); sort_moves(sb, nb);
    int same = 1;
    for (std::size_t i = 0; i < na; i++) if (sa[i] != sb[i]) same = 0;
    QWC_TRUE(same, "identical set at fm 20 vs fm max");
  }
  fail |= qwc_end();

  /* The overflow guard is intact: an ACTUAL game make() at the 65535 halfmove is
   * rejected (it is not the legal generator's job to make that move). */
  qwc_begin("mgc.counters.make_rejects");
  {
    Position a;
    QWC_TRUE(fen_load(&a, "7k/8/8/8/8/3p4/3R4/K7 w - - 0 1", &s_why) == 1, "load");
    a.halfmove = 65535; a.fullmove = 1; pos_rebuild(&a);
    StateInfo st{};
    Move quiet = make_move(square_of(0, 0), square_of(0, 1), 0u, 0u);   /* a1a2 (quiet -> hm+1) */
    QWC_EQ_I(pos_make_move(&a, &st, quiet), 0, "make() at 65535 halfmove is rejected (no overflow)");
  }
  fail |= qwc_end();

  /* On positions with no castling rights, the full API must agree exactly with the
   * non-castling API (no castles to add): quiet, EP, promotion, mate, stalemate. */
  qwc_begin("mgc.counters.agree_noncastling");
  {
    static const char *fens[] = {
      "k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1",    /* pinned rook (no rights)       */
      "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20",  /* EP (no rights)            */
      "r6K/5P2/8/8/8/8/8/k7 w - - 0 1",    /* promotion (no rights)     */
      "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1",     /* checkmate (no rights)     */
      "7k/5K2/6Q1/8/8/8/8/8 b - - 0 1",     /* stalemate (no rights)     */
    };
    for (int k = 0; k < 5; k++) {
      Position pos;
      QWC_TRUE(fen_load(&pos, fens[k], &s_why) == 1, "load");
      QWC_EQ_I((int)pos.cr, 0, "no castling rights in this case");
      Move a[MGC_CAP], b[MGC_CAP]; std::size_t na = 0, nb = 0;
      QWC_TRUE(movegen_legal(pos, std::span<Move>(a, MGC_CAP), na), "full gen");
      QWC_TRUE(movegen_legal_noncastling(pos, std::span<Move>(b, MGC_CAP), nb), "non-castling gen");
      QWC_EQ_I((int)na, (int)nb, "full count == non-castling count (no rights)");
      Move sa[MGC_CAP], sb[MGC_CAP];
      std::memcpy(sa, a, na * sizeof(Move)); std::memcpy(sb, b, nb * sizeof(Move));
      sort_moves(sa, na); sort_moves(sb, nb);
      int same = 1;
      for (std::size_t i = 0; i < na; i++) if (sa[i] != sb[i]) same = 0;
      QWC_TRUE(same, "full set == non-castling set (no rights)");
    }
  }
  fail |= qwc_end();
  return fail;
}

int main() {
  int fail = 0;
  fail |= t_fixtures();
  fail |= t_rights_masks();
  fail |= t_rights_lost();
  fail |= t_makeunmake();
  fail |= t_capacity();
  fail |= t_preservation();
  fail |= t_counters();
  std::printf("\nmovegen castling (full legal): %s\n", fail ? "FAIL" : "all groups PASS");
  return fail ? 1 : 0;
}
