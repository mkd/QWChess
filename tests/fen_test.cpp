/* QwenChess T003 checkpoint-2 test: strict FEN load/emit.
 *
 * Exercises the FEN contract against the one Position:
 *   - round-trip (load -> emit -> load) for canonical boards, asserting both a
 *     byte-identical re-emit and a field-for-field identical reloaded Position;
 *   - fielded rejection of malformed and internally inconsistent FENs (the
 *     caller's Position must be left untouched on failure);
 *   - the en-passant contract, using the provided fixtures *as verified by the
 *     engine* plus clean, unambiguous cases and reflections; the position key is
 *     recomputed independently via the core Zobrist hasher;
 *   - emission buffer boundaries (succeeds at FEN_BUF_SIZE, fails + leaves the
 *     buffer untouched when too small).
 *
 * Checks are always active (never NDEBUG-guarded); the harness reports aggregate
 * counts and per-failure diagnostics. FEN parsing is a representation concern;
 * move legality / reachability are later checkpoints.
 */
#include "position/fen.hpp"
#include "position/position.hpp"
#include "core/types.hpp"
#include "core/square.hpp"
#include "core/zobrist.hpp"
#include "test_util.hpp"
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <array>
#include <algorithm>
#include <span>
#include <string_view>

using namespace qwc;

/* Field-for-field equality of two Positions (inputs + derived caches). */
static int pos_equal(const Position *a, const Position *b) {
  if (a->mailbox != b->mailbox) return 0;
  if (a->side != b->side || a->cr != b->cr || a->ep_sq != b->ep_sq) return 0;
  if (a->halfmove != b->halfmove || a->fullmove != b->fullmove) return 0;
  for (int p = 0; p < PIECE_NB; p++) if (a->by_piece[p] != b->by_piece[p]) return 0;
  for (int c = 0; c < COLOR_NB; c++) if (a->by_color[c] != b->by_color[c]) return 0;
  if (a->occ != b->occ) return 0;
  if (a->white_king_sq != b->white_king_sq || a->black_king_sq != b->black_king_sq) return 0;
  if (a->key != b->key) return 0;
  return 1;
}

/* The canonical valid FEN shared by the view/diagnostics tests. */
static const char *good_fen() {
  return "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
}

/* Build the top-bottom color reflection of `src` into `dst`: every piece at
 * (file f, rank r) moves to (f, 7-r) with its color flipped (XOR 8 = the color
 * bit, since white pieces are type 1..6 and black are type+8). The ep square and
 * the side reflect the same way; castling and the counters carry over (the
 * fixtures have no castling). Then rebuild the derived caches. Used to assert the
 * geometry-symmetric property that a rank-reflect/color-swap ep keeps the same
 * canonical file and the same legality. */
static void pos_mirror(const Position *src, Position *dst) {
  dst->mailbox = {};
  for (int s = 0; s < SQ_NB; s++) {
    int p = to_under(src->mailbox[s]);
    if (!p) continue;
    int f = file_of((Square)s), r = rank_of((Square)s);
    dst->mailbox[square_of(f, 7 - r)] = (Piece)(p ^ 8);
  }
  dst->side = (Color)(1 - to_under(src->side));
  dst->cr   = src->cr;
  if (src->ep_sq < SQ_NB) {
    int f = file_of(src->ep_sq), r = rank_of(src->ep_sq);
    dst->ep_sq = square_of(f, 7 - r);
  } else dst->ep_sq = NO_SQUARE;
  dst->halfmove = src->halfmove;
  dst->fullmove = src->fullmove;
  pos_rebuild(dst);
}

/* True iff the en-passant field (the 4th field of `fen`) names the square `sq`,
 * or "-" for NO_SQUARE. Locates the field after the 3rd space (board/side/
 * castling) and compares its text to the square's standard name. Used to verify
 * that emission preserves the RECORDED ep target, not a canonicalized one. */
static int ep_field_is(const char *fen, int sq) {
  const char *p = fen;
  int nf = 0;
  /* Advance past every char; once we have cleared the 3rd field-separator
   * (after board, side, castling) we are at the start of the ep field. */
  while (*p) {
    if (*p == ' ' && ++nf == 3) { p++; break; }
    p++;
  }
  if (nf != 3) return 0;                 /* fewer than 4 fields: no ep field */
  /* The ep token ends at the next space or NUL. */
  const char *e = p;
  while (*e && *e != ' ') e++;
  if (sq >= SQ_NB)
    return (e - p) == 1 && *p == '-';
  return (e - p) == 2 &&
         p[0] == (char)('a' + (sq & 7)) &&
         p[1] == (char)('1' + (sq >> 3));
}

/* ---- round-trip: start, a castling mid-game, a full-type board ---- */
static int t_roundtrip() {
  qwc_begin("fen.roundtrip");
  static const char *boards[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 3",
    "rbnqkbnr/p6p/8/8/8/8/P6P/RBNQKBNR w KQkq - 0 1",
  };
  char buf[FEN_BUF_SIZE];
  for (int i = 0; i < 3; i++) {
    const char *why = nullptr;
    Position a, b;
    int ok = fen_load(&a, boards[i], &why);
    QWC_TRUE(ok, "roundtrip: load");
    if (!ok) continue;
    std::memset(buf, 0xAA, sizeof buf);                 /* poison: detect truncation */
    QWC_TRUE(fen_emit(&a, buf, FEN_BUF_SIZE), "roundtrip: emit");
    QWC_TRUE(std::strcmp(buf, boards[i]) == 0, "roundtrip: byte-identical re-emit");
    QWC_TRUE(fen_load(&b, buf, &why), "roundtrip: reload");
    QWC_TRUE(pos_equal(&a, &b), "roundtrip: reloaded Position identical");
  }
  return qwc_end();
}

/* ---- fielded rejection: the caller's Position must survive untouched ---- */
static int t_reject() {
  qwc_begin("fen.reject");
  const char *why = nullptr;
  Position sentinel;
  pos_set_startpos(&sentinel);

  /* Each of these must be rejected with a reason, and must leave *pos exactly
   * as it was (a sentinel startpos here). */
  static const char *bad[] = {
    /* -- field count / spacing -- */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0",    /* 5 fields */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 2", /* 7 fields */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR  w KQkq - 0 1",  /* double space */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 ",  /* trailing space */
    /* -- placement -- */
    "rnbqkbnr/ppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",    /* rank 7 wide */
    "rnbqkbnr/8/8/8/8/8/8/PPPPPPPP w KQkq - 0 1",                 /* 7 ranks */
    "rnbqkbnx/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",   /* bad letter */
    "rnbqkbnr/ppppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",  /* rank 9 wide */
    /* -- side / castling -- */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1",   /* bad side */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQKq - 0 1",   /* duplicate K */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkz - 0 1",   /* bad letter */
    /* -- en-passant: structurally invalid record -- */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq d5 0 1",  /* wrong rank */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq d6 0 1",  /* no pawn on d5 */
    /* -- counters: non-canonical / out of range -- */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 00 1",  /* leading zero */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 0",   /* fullmove 0 */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 65536 1", /* beyond u16 */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - -1 1",  /* sign */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1.5", /* non-digit */
    /* -- illegal boards (pos_validate) -- */
    "P6k/8/8/8/8/8/8/K6 w - - 0 1",           /* white pawn on rank 8 */
    "1nbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", /* CR_BQ unbacked (a8 empty) */
    "8/8/8/8/8/8/4k3/4K3 w - - 0 1",       /* adjacent kings e1/e2 */
  };
  for (std::size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    Position p = sentinel;
    QWC_TRUE(fen_load(&p, bad[i], &why) == 0, "rejected");
    QWC_TRUE(why != nullptr && why[0] != '\0', "rejection has a reason");
    QWC_TRUE(pos_equal(&p, &sentinel), "caller Position untouched");
  }
  return qwc_end();
}

/* ---- the en-passant contract: the eight task fixtures, verbatim ----
 *
 * These eight FENs are the regression's fixed inputs (standard castling "-" is
 * irrelevant here). Every one is a VALID position that must LOAD: a recorded
 * ep target is retained even when no capture is king-safe. The expectations are
 * fixed by the chess position, not read back from the engine:
 *   - placement is checked against an independently-listed set of pieces (every
 *     other square empty);
 *   - `recorded` is the exact ep square the loader must retain (43 = d6, 45 = f6);
 *   - `canon` is the file the key uses: 3 (d-file) iff a capture is legal, else -1;
 *   - `legal` is pos_ep_is_legal (at least one captor is king-safe);
 *   - `caps` are the SPECIFIC captors to verify (a non-captor square is 0):
 *       #1 e5 legal; #2 e5 pinned; #3 g5 exposes the king; #4 e5 clears a check;
 *       #5 c5 legal / e5 pinned (so "c5d6 only"); #6 neither; #7/#8 e5 unsafe.
 * Square indices are 0-based file + 8*rank (d6 = 43, d5 = 35, e5 = 36, ...). */
typedef struct { Piece piece; int file; int rank; } Pl;       /* listed placement */
typedef struct { int captor; int safe; } Captor;           /* one captor check */
typedef struct {
  int         id;
  const char *fen;
  Pl          white[8]; int wn;
  Pl          black[8]; int bn;
  int         recorded;   /* recorded ep square, or NO_SQUARE */
  int         canon;      /* 3 (d-file) if legal, else -1 (absent) */
  int         legal;      /* pos_ep_is_legal */
  Captor      caps[2]; int ncap;
} EpCase;
static int t_ep() {
  /* The eight task fixtures, verbatim. All are valid and must load. The
   * expectations (placement lists, recorded ep, canonical file, legality,
   * specific captors) are fixed by the chess position -- see the struct
   * comment above. No expectation is derived from engine output. */
  static const EpCase cases[] = {
    { 1, "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20",
      { {Piece::W_KING,4,0},{Piece::W_PAWN,4,4} }, 2, { {Piece::B_KING,0,7},{Piece::B_PAWN,3,4} }, 2,
      43, 3, 1, { {36,1} }, 1 },
    { 2, "k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 20",
      { {Piece::W_KING,4,0},{Piece::W_PAWN,4,4} }, 2, { {Piece::B_KING,0,7},{Piece::B_ROOK,4,7},{Piece::B_PAWN,3,4} }, 3,
      43, -1, 0, { {36,0} }, 1 },
    { 3, "k7/8/8/r4pPK/8/8/8/8 w - f6 0 20",
      { {Piece::W_KING,7,4},{Piece::W_PAWN,6,4} }, 2, { {Piece::B_KING,0,7},{Piece::B_ROOK,0,4},{Piece::B_PAWN,5,4} }, 3,
      45, -1, 0, { {38,0} }, 1 },
    { 4, "k7/8/8/3pP3/4K3/8/8/8 w - d6 0 20",
      { {Piece::W_KING,4,3},{Piece::W_PAWN,4,4} }, 2, { {Piece::B_KING,0,7},{Piece::B_PAWN,3,4} }, 2,
      43, 3, 1, { {36,1} }, 1 },
    { 5, "k3r3/8/8/2PpP3/8/8/8/4K3 w - d6 0 20",
      { {Piece::W_KING,4,0},{Piece::W_PAWN,2,4},{Piece::W_PAWN,4,4} }, 3, { {Piece::B_KING,0,7},{Piece::B_ROOK,4,7},{Piece::B_PAWN,3,4} }, 3,
      43, 3, 1, { {34,1},{36,0} }, 2 },
    { 6, "k7/8/8/3p4/8/8/8/4K3 w - d6 0 20",
      { {Piece::W_KING,4,0} }, 1, { {Piece::B_KING,0,7},{Piece::B_PAWN,3,4} }, 2,
      43, -1, 0, { {34,0},{36,0} }, 2 },
    { 7, "kb6/8/8/4Pp2/8/8/7K/8 w - f6 0 20",
      { {Piece::W_KING,7,1},{Piece::W_PAWN,4,4} }, 2, { {Piece::B_KING,0,7},{Piece::B_BISHOP,1,7},{Piece::B_PAWN,5,4} }, 3,
      45, -1, 0, { {36,0} }, 1 },
    { 8, "4b2k/8/8/3pP3/K7/8/8/8 w - d6 0 20",
      { {Piece::W_KING,0,3},{Piece::W_PAWN,4,4} }, 2, { {Piece::B_KING,7,7},{Piece::B_BISHOP,4,7},{Piece::B_PAWN,3,4} }, 3,
      43, -1, 0, { {36,0} }, 1 },
  };
  int fail = 0;

  /* -- originals: load, placement, recorded ep, legality, canon, captors, key -- */
  qwc_begin("fen.ep");
  for (std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    const EpCase *c = &cases[i];
    Position p; const char *why = nullptr;
    QWC_TRUE(fen_load(&p, c->fen, &why), "load (all eight must load)");
    std::array<Piece, SQ_NB> exp{};
    for (int k = 0; k < c->wn; k++)
      exp[square_of(c->white[k].file, c->white[k].rank)] = c->white[k].piece;
    for (int k = 0; k < c->bn; k++)
      exp[square_of(c->black[k].file, c->black[k].rank)] = c->black[k].piece;
    QWC_TRUE(p.mailbox == exp, "placement == listed pieces (rest empty)");
    QWC_TRUE((int)p.ep_sq == c->recorded, "recorded ep target retained");
    QWC_EQ_I(pos_ep_is_legal(&p), c->legal, "legality");
    QWC_EQ_I(pos_canon_ep_file(&p), c->canon, "canonical file");
    for (int k = 0; k < c->ncap; k++)
      QWC_EQ_I(pos_ep_capture_is_safe(&p, (Square)c->caps[k].captor), c->caps[k].safe,
               "captor king-safe");
    QWC_EQ_U64(pos_key(&p), zobrist_compute(p.mailbox.data(), p.side, p.cr, c->canon),
               "key == zobrist contract (canonical ep file)");
  }
  /* A *legal* ep changes the key by exactly the target-file key; an absent
   * canonical ep leaves it unchanged (XOR difference zero). */
  {
    Position lp, lp_ne; const char *why = nullptr;
    QWC_TRUE(fen_load(&lp, cases[0].fen, &why), "key-diff: legal-ep load");
    lp_ne = lp; lp_ne.ep_sq = NO_SQUARE;
    QWC_EQ_U64(pos_key(&lp) ^ pos_key(&lp_ne), zobrist_ep_file(cases[0].canon),
               "legal-ep key XOR-diff == target-file key");
    Position ap, ap_ne;
    QWC_TRUE(fen_load(&ap, cases[1].fen, &why), "key-diff: absent-ep load");
    ap_ne = ap; ap_ne.ep_sq = NO_SQUARE;
    QWC_EQ_U64(pos_key(&ap) ^ pos_key(&ap_ne), 0, "absent-ep key XOR-diff == 0");
  }
  fail |= qwc_end();

  /* -- mirrors: rank-reflect + color-swap (black to move) keep canon + legality -- */
  qwc_begin("fen.ep.mirror");
  for (std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    const EpCase *c = &cases[i];
    Position a, m; const char *why = nullptr;
    QWC_TRUE(fen_load(&a, c->fen, &why), "mirror: source loads");
    pos_mirror(&a, &m);
    const char *vw = nullptr;
    QWC_TRUE(pos_validate(&m, &vw), "mirror: reflection is a valid position");
    QWC_EQ_I(pos_canon_ep_file(&m), pos_canon_ep_file(&a), "mirror: canon unchanged vs source");
    QWC_EQ_I(pos_ep_is_legal(&m), pos_ep_is_legal(&a), "mirror: legality unchanged vs source");
    QWC_EQ_I(pos_canon_ep_file(&m), c->canon, "mirror: canon == fixed expectation");
    QWC_EQ_I(pos_ep_is_legal(&m), c->legal, "mirror: legality == fixed expectation");
  }
  fail |= qwc_end();

  /* -- emission + reload preserve the recorded target and the logical state -- */
  qwc_begin("fen.ep.roundtrip");
  for (std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    const EpCase *c = &cases[i];
    Position a, b; const char *why = nullptr;
    char buf[FEN_BUF_SIZE];
    QWC_TRUE(fen_load(&a, c->fen, &why), "rt: load");
    QWC_TRUE(fen_emit(&a, buf, FEN_BUF_SIZE), "rt: emit");
    QWC_TRUE(ep_field_is(buf, c->recorded), "rt: emitted ep field == recorded target");
    QWC_TRUE(fen_load(&b, buf, &why), "rt: reload");
    QWC_TRUE(pos_equal(&a, &b), "rt: reloaded Position identical (incl. ep)");
  }
  fail |= qwc_end();

  return fail;
}

/* ---- emission buffer boundaries ---- */
static int t_emit_bounds() {
  qwc_begin("fen.emit.bounds");
  Position p;
  const char *why = nullptr;
  const char *start = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  QWC_TRUE(fen_load(&p, start, &why), "emit.bounds: load");
  char big[FEN_BUF_SIZE];
  QWC_TRUE(fen_emit(&p, big, FEN_BUF_SIZE), "emit.bounds: fits FEN_BUF_SIZE");
  QWC_TRUE(std::strcmp(big, start) == 0, "emit.bounds: correct text");
  std::array<char, 16> small{};
  for (int i = 0; i < 16; i++) small[i] = (char)0x5A;   /* poison */
  QWC_EQ_I(fen_emit(&p, small.data(), (int)small.size()), 0, "emit.bounds: too small -> 0");
  int untouched = 1;
  for (int i = 0; i < 16; i++) if (small[i] != (char)0x5A) untouched = 0;
  QWC_TRUE(untouched, "emit.bounds: buffer unchanged when too small");
  QWC_EQ_I(fen_emit(&p, nullptr, FEN_BUF_SIZE), 0, "emit.bounds: nullptr out -> 0");
  QWC_EQ_I(fen_emit(&p, big, 0), 0, "emit.bounds: zero size -> 0");
  return qwc_end();
}

/* ---- the bounded (std::string_view) load contract -------------------------
 * The parser is sized by the view, not by a NUL terminator. These cases exercise
 * that boundary: a valid FEN that is non-terminated / a slice of a larger buffer
 * must load; empty, NUL-bearing, truncated and trailing-junk views must be
 * rejected, leaving the caller's Position (and its history head) untouched. */
static int t_view_boundaries() {
  qwc_begin("fen.view.load");
  Position sentinel; pos_set_startpos(&sentinel);
  const char *fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  const std::size_t len = std::strlen(fen);
  Position clean; const char *why = nullptr;
  QWC_TRUE(fen_load(&clean, fen, &why), "view: clean reference load");

  /* (a) a valid FEN whose buffer is NOT NUL-terminated at the view boundary
   * (junk, not a NUL, immediately after the FEN) loads from the view alone. */
  {
    char buf[128];
    std::memcpy(buf, fen, len);
    std::memcpy(buf + len, "JUNKJUNK", 8);   /* no NUL at the view's end */
    Position p = sentinel;
    std::string_view err;
    QWC_TRUE(fen_load(p, std::string_view(buf, len), &err), "view: non-terminated valid FEN loads");
    QWC_TRUE(err.empty(), "view: success leaves error empty");
    QWC_TRUE(pos_equal(&p, &clean), "view: identical to a clean load");
  }

  /* (b) a valid FEN that is a slice of a larger buffer (junk before and after)
   * loads from the slice. */
  {
    char buf[160];
    const std::size_t pre = 10;
    std::memcpy(buf, "JUNKPREFIX", pre);
    std::memcpy(buf + pre, fen, len);
    std::memcpy(buf + pre + len, "JUNKSUFFIX", 10);
    Position p = sentinel;
    std::string_view err;
    QWC_TRUE(fen_load(p, std::string_view(buf + pre, len), &err), "view: slice inside surrounding junk loads");
    QWC_TRUE(err.empty(), "view: success leaves error empty");
  }

  /* (c) empty and default-constructed views are rejected. */
  for (int k = 0; k < 2; k++) {
    Position p = sentinel;
    std::string_view err;
    bool ok = k == 0 ? fen_load(p, std::string_view{}, &err)
                     : fen_load(p, std::string_view(), &err);
    QWC_TRUE(!ok, "view: empty / default view rejected");
    QWC_TRUE(!err.empty(), "view: rejection has a reason");
    QWC_TRUE(pos_equal(&p, &sentinel) && p.history == sentinel.history, "view: Position+history preserved");
  }

  /* (d) a NUL byte inside the supplied range is input data and is rejected; the
   * same FEN without the embedded NUL still loads. */
  {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s", fen);   /* valid, NUL-terminated */
    buf[len / 2] = '\0';                          /* corrupt with an embedded NUL */
    Position p = sentinel;
    std::string_view err;
    QWC_TRUE(!fen_load(p, std::string_view(buf, len), &err), "view: embedded NUL rejected");
    QWC_TRUE(!err.empty(), "view: embedded NUL has a reason");
    QWC_TRUE(pos_equal(&p, &sentinel) && p.history == sentinel.history, "view: Position+history preserved");
    p = sentinel; err = std::string_view{};
    QWC_TRUE(fen_load(p, std::string_view(good_fen()), &err), "view: the un-corrupted FEN loads");
  }

  /* (e) a view that accidentally *includes* the terminating NUL is rejected; the
   * identical view excluding it is the valid FEN. */
  {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s", fen);
    Position p = sentinel;
    std::string_view err;
    QWC_TRUE(!fen_load(p, std::string_view(buf, len + 1), &err), "view: included terminal NUL rejected");
    QWC_TRUE(!err.empty(), "view: terminal NUL has a reason");
    p = sentinel; err = std::string_view{};
    QWC_TRUE(fen_load(p, std::string_view(buf, len), &err), "view: terminator-excluding view loads");
  }

  /* (f) truncation (a view that ends mid-FEN) and trailing junk are rejected. */
  {
    Position p = sentinel;
    std::string_view err;
    QWC_TRUE(!fen_load(p, std::string_view(fen, len - 1), &err), "view: truncated FEN rejected");
    QWC_TRUE(!err.empty(), "view: truncated FEN has a reason");
    QWC_TRUE(pos_equal(&p, &sentinel) && p.history == sentinel.history, "view: Position+history preserved");
    char buf[96];
    std::memcpy(buf, fen, len);
    std::memcpy(buf + len, "junk", 4);
    p = sentinel; err = std::string_view{};
    QWC_TRUE(!fen_load(p, std::string_view(buf, len + 4), &err), "view: trailing junk rejected");
    QWC_TRUE(!err.empty(), "view: trailing junk has a reason");
  }

  return qwc_end();
}

/* ---- the bounded (std::span<char>) emission contract ----------------------- */
static int t_emit_span() {
  qwc_begin("fen.emit.span");
  Position p; const char *why = nullptr;
  const char *start = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  QWC_TRUE(fen_load(&p, start, &why), "emit.span: load");
  const std::size_t len = std::strlen(start);   /* rendered length, no terminator */

  /* exact text-length + 1 capacity succeeds (room for the terminator). */
  {
    std::array<char, 128> out{};
    QWC_TRUE(fen_emit(p, std::span<char>(out.data(), len + 1)), "span: exact+1 succeeds");
    QWC_TRUE(std::memcmp(out.data(), start, len) == 0 && out[len] == '\0', "span: correct text + terminator");
  }
  /* one byte short (no room for the NUL) fails and leaves the buffer untouched. */
  {
    std::array<char, 128> out{};
    for (auto &c : out) c = (char)0x5A;   /* poison */
    QWC_TRUE(!fen_emit(p, std::span<char>(out.data(), len)), "span: one byte short fails");
    int untouched = 1;
    for (auto c : out) if (c != (char)0x5A) untouched = 0;
    QWC_TRUE(untouched, "span: buffer unchanged when one byte short");
  }
  /* empty output (and a default-constructed span) fails. */
  {
    char b;
    QWC_TRUE(!fen_emit(p, std::span<char>(&b, 0)), "span: empty output fails");
    QWC_TRUE(!fen_emit(p, std::span<char>{}), "span: default-constructed span fails");
  }
  /* an invalid Position fails (a pos_reset board has no kings). */
  {
    Position bad; pos_reset(&bad);
    char b[8];
    QWC_TRUE(!fen_emit(bad, std::span<char>(b, sizeof b)), "span: invalid Position fails");
  }
  /* canary + unchanged suffix: emit into a larger buffer; the unused suffix stays. */
  {
    std::array<char, 128> out{};
    for (auto &c : out) c = (char)0x77;   /* poison the whole buffer */
    QWC_TRUE(fen_emit(p, std::span<char>(out.data(), out.size())), "span: into a larger buffer succeeds");
    QWC_TRUE(std::memcmp(out.data(), start, len) == 0 && out[len] == '\0', "span: text + terminator written");
    int suffix_ok = 1;
    for (std::size_t i = len + 1; i < out.size(); i++) if (out[i] != (char)0x77) suffix_ok = 0;
    QWC_TRUE(suffix_ok, "span: unused suffix left unchanged");
  }
  /* the legacy adapter keeps its null / nonpositive handling. */
  {
    char b[8];
    QWC_EQ_I(fen_emit(&p, nullptr, FEN_BUF_SIZE), 0, "adapter: null out -> 0");
    QWC_EQ_I(fen_emit(&p, b, 0), 0, "adapter: zero size -> 0");
    QWC_EQ_I(fen_emit(&p, b, -1), 0, "adapter: negative size -> 0");
    QWC_EQ_I(fen_emit(nullptr, b, FEN_BUF_SIZE), 0, "adapter: null pos -> 0");
  }
  return qwc_end();
}

/* ---- diagnostics: cleared on success / static-backed on failure / optional -- */
static int t_diagnostics() {
  qwc_begin("fen.diagnostics");
  Position sentinel; pos_set_startpos(&sentinel);
  const char *bad = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1";   /* bad side */

  /* a pre-set error view is cleared on success. */
  {
    Position p;
    std::string_view err = "stale-diagnostics-should-be-cleared";
    QWC_TRUE(fen_load(p, std::string_view(good_fen()), &err), "diag: valid loads");
    QWC_TRUE(err.empty(), "diag: error cleared on success");
  }
  /* a failure supplies a nonempty, static-backed reason; the pointer is optional. */
  {
    Position p;
    std::string_view err;
    QWC_TRUE(!fen_load(p, std::string_view(bad), &err), "diag: invalid rejected");
    QWC_TRUE(!err.empty(), "diag: failure has a nonempty reason");
    QWC_TRUE(err.data()[0] != '\0', "diag: reason is static-backed (survives the call)");
    Position p2 = sentinel;
    QWC_TRUE(!fen_load(p2, std::string_view(bad)), "diag: error pointer is optional");
    QWC_TRUE(pos_equal(&p2, &sentinel) && p2.history == sentinel.history, "diag: Position+history preserved");
  }
  /* the legacy adapter keeps its null-`why` handling (does not crash). */
  {
    Position p = sentinel;
    QWC_EQ_I(fen_load(&p, bad, nullptr), 0, "adapter: null why -> 0 (no crash)");
    QWC_EQ_I(fen_load(&p, good_fen(), nullptr), 1, "adapter: null why -> 1 on success");
  }
  return qwc_end();
}

/* ---- a round-trip through BOTH bounded interfaces (string_view load + span
 * emit) must reproduce the canonical FEN byte-for-byte and reload identically.
 * (The pointer adapters in t_roundtrip exercise the same primaries; this pins the
 * two bounded forms end-to-end.) */
static int t_view_roundtrip() {
  qwc_begin("fen.view.roundtrip");
  const char *fen = good_fen();   /* canonical: normalized castling order + runs */
  const std::size_t len = std::strlen(fen);
  char in[128];
  std::memcpy(in, fen, len + 1);   /* a NUL-terminated source buffer */
  Position p;
  std::string_view err;
  QWC_TRUE(fen_load(p, std::string_view(in, len), &err), "rt: string_view load (terminator excluded)");
  QWC_TRUE(err.empty(), "rt: success leaves error empty");
  char out[128];
  for (auto &c : out) c = (char)0x5A;   /* poison, to confirm a full overwrite */
  QWC_TRUE(fen_emit(p, std::span<char>(out, sizeof out)), "rt: span emit");
  QWC_TRUE(std::memcmp(out, fen, len) == 0 && out[len] == '\0', "rt: emitted text == input + NUL");
  Position p2;
  QWC_TRUE(fen_load(p2, std::string_view(out, len), &err), "rt: reload the emitted text");
  QWC_TRUE(pos_equal(&p, &p2), "rt: reloaded Position identical");
  return qwc_end();
}

int main() {
  int fail = 0;
  fail |= t_roundtrip();
  fail |= t_reject();
  fail |= t_ep();
  fail |= t_emit_bounds();
  fail |= t_view_boundaries();
  fail |= t_view_roundtrip();
  fail |= t_emit_span();
  fail |= t_diagnostics();
  return fail ? 1 : 0;
}
