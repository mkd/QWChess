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
#include "position/fen.h"
#include "position/position.h"
#include "core/types.h"
#include "core/square.h"
#include "core/zobrist.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>

/* Field-for-field equality of two Positions (inputs + derived caches). */
static int pos_equal(const Position *a, const Position *b) {
  if (memcmp(a->mailbox, b->mailbox, SQ_NB) != 0) return 0;
  if (a->side != b->side || a->cr != b->cr || a->ep_sq != b->ep_sq) return 0;
  if (a->halfmove != b->halfmove || a->fullmove != b->fullmove) return 0;
  for (int p = 0; p < PIECE_NB; p++) if (a->byPiece[p] != b->byPiece[p]) return 0;
  for (int c = 0; c < COLOR_NB; c++) if (a->byColor[c] != b->byColor[c]) return 0;
  if (a->occ != b->occ) return 0;
  if (a->wKingSq != b->wKingSq || a->bKingSq != b->bKingSq) return 0;
  if (a->key != b->key) return 0;
  return 1;
}

/* ---- round-trip: start, a castling mid-game, a full-type board ---- */
static int t_roundtrip(void) {
  qwc_begin("fen.roundtrip");
  static const char *boards[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 3",
    "rbnqkbnr/p6p/8/8/8/8/P6P/RBNQKBNR w KQkq - 0 1",
  };
  char buf[FEN_BUF_SIZE];
  for (int i = 0; i < 3; i++) {
    const char *why = NULL;
    Position a, b;
    int ok = fen_load(&a, boards[i], &why);
    QWC_TRUE(ok, "roundtrip: load");
    if (!ok) continue;
    memset(buf, 0xAA, sizeof buf);                 /* poison: detect truncation */
    QWC_TRUE(fen_emit(&a, buf, FEN_BUF_SIZE), "roundtrip: emit");
    QWC_TRUE(strcmp(buf, boards[i]) == 0, "roundtrip: byte-identical re-emit");
    QWC_TRUE(fen_load(&b, buf, &why), "roundtrip: reload");
    QWC_TRUE(pos_equal(&a, &b), "roundtrip: reloaded Position identical");
  }
  return qwc_end();
}

/* ---- fielded rejection: the caller's Position must survive untouched ---- */
static int t_reject(void) {
  qwc_begin("fen.reject");
  const char *why = NULL;
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
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 101 1", /* halfmove>100 */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - -1 1",  /* sign */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1.5", /* non-digit */
    /* -- illegal boards (pos_validate) -- */
    "P6k/8/8/8/8/8/8/K6 w - - 0 1",           /* white pawn on rank 8 */
    "1nbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", /* CR_BQ unbacked (a8 empty) */
    "8/8/8/8/8/8/4k3/4K3 w - - 0 1",       /* adjacent kings e1/e2 */
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    Position p = sentinel;
    QWC_TRUE(fen_load(&p, bad[i], &why) == 0, "rejected");
    QWC_TRUE(why != NULL && why[0] != '\0', "rejection has a reason");
    QWC_TRUE(pos_equal(&p, &sentinel), "caller Position untouched");
  }
  return qwc_end();
}

/* ---- the en-passant contract, as verified by the engine ----
 *
 * The eight fixtures the task supplied carry labels that do not all match the
 * engine (and two of them are not valid positions). We assert the *verified*
 * behavior instead, plus clean unambiguous cases and black-to-move reflections.
 * `ep` is the RECORDED square the loader must retain; `canon` is the file the
 * key must reflect (3 = d-file when the capture is legal, -1 when not). */
typedef struct { const char *fen; int load; int legal; int canon; Square ep; } EpCase;

/* Square indices (file + 8*rank, 0-based): d6 = 3 + 8*5 = 43; d3 = 3 + 8*2 = 19. */
static int t_ep(void) {
  qwc_begin("fen.ep");
  static const EpCase cases[] = {
    /* the eight provided fixtures (verified: 5 capturable, 1 uncapturable, 2 invalid) */
    { "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20",     1, 1,  3, 43 }, /* legal (captor e5) */
    { "4k3/8/8/3pP1K1/8/8/8/8 w - d6 0 20",   1, 1,  3, 43 }, /* legal */
    { "r5k1/8/8/3pPp2/6K1/8/8/8 w - d6 0 20",1, 0, -1, 43 }, /* uncapturable (f5 checks king) */
    { "k7/8/8/3pP3/4K3/8/8/8 w - d6 0 20",     1, 1,  3, 43 }, /* legal */
    { "k3r3/8/8/2PpP3/8/8/8/4K3 w - d6 0 20", 1, 1, 3, 43 }, /* one of two captors works */
    { "2kr4/8/8/3pP3/8/8/8/8 w - d6 0 20",   0, 0, -1, NO_SQUARE }, /* no white king */
    { "8/8/3k1p2/3pP3/8/8/8/4K3 w - d6 0 20", 0, 0, -1, NO_SQUARE }, /* ep target occupied */
    { "3rk3/8/8/3pP3/8/8/8/4K3 w - d6 0 20", 1, 1, 3, 43 }, /* legal */
    /* clean, unambiguous cases */
    { "k7/8/8/3p4/8/8/8/4K3 w - d6 0 20",   1, 0, -1, 43 }, /* no captor */
    { "k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 20", 1, 0, -1, 43 }, /* single captor pinned */
    { "k7/8/8/q1PpP3/8/8/8/4K3 w - d6 0 20", 1, 0, -1, 43 }, /* two captors, neither escapes check */
    /* color- and rank-reflects (black to move, ep on d3) */
    { "7k/8/8/8/3Pp3/8/8/K7 b - d3 0 20",  1, 1,  3, 19 }, /* reflected: legal */
    { "7k/8/8/8/3P4/8/8/K7 b - d3 0 20",  1, 0, -1, 19 }, /* reflected: no captor */
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    const EpCase *c = &cases[i];
    Position p;
    const char *why = NULL;
    int ok = fen_load(&p, c->fen, &why);
    QWC_EQ_I(ok, c->load, "ep: load");
    if (c->load) {
      QWC_TRUE((int)p.ep_sq == (int)c->ep, "ep: recorded square retained");
      QWC_EQ_I(pos_ep_is_legal(&p), c->legal, "ep: legality");
      QWC_EQ_I(pos_canon_ep_file(&p), c->canon, "ep: canonical file");
      QWC_EQ_U64(pos_key(&p),
                 zobrist_compute(p.mailbox, p.side, p.cr, c->canon),
                 "ep: key == independent recompute");
    } else {
      QWC_TRUE(why != NULL && why[0] != '\0', "ep: rejection has a reason");
    }
  }

  /* the key must change when a *legal* ep square is present versus absent */
  {
    Position p;
    const char *why = NULL;
    QWC_TRUE(fen_load(&p, "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20", &why), "ep: key-compare load");
    u64 with = pos_key(&p);
    p.ep_sq = NO_SQUARE;
    u64 without = pos_key(&p);
    QWC_TRUE(with != without, "ep: legal ep changes the key");
    QWC_EQ_U64(with,    zobrist_compute(p.mailbox, p.side, p.cr, 3),  "ep: with-ep key");
    QWC_EQ_U64(without, zobrist_compute(p.mailbox, p.side, p.cr, -1), "ep: no-ep key");
  }
  return qwc_end();
}

/* ---- emission buffer boundaries ---- */
static int t_emit_bounds(void) {
  qwc_begin("fen.emit.bounds");
  Position p;
  const char *why = NULL;
  const char *start = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  QWC_TRUE(fen_load(&p, start, &why), "emit.bounds: load");
  char big[FEN_BUF_SIZE];
  QWC_TRUE(fen_emit(&p, big, FEN_BUF_SIZE), "emit.bounds: fits FEN_BUF_SIZE");
  QWC_TRUE(strcmp(big, start) == 0, "emit.bounds: correct text");
  char small[16];
  for (int i = 0; i < 16; i++) small[i] = (char)0x5A;   /* poison */
  QWC_EQ_I(fen_emit(&p, small, (int)sizeof small), 0, "emit.bounds: too small -> 0");
  int untouched = 1;
  for (int i = 0; i < 16; i++) if (small[i] != (char)0x5A) untouched = 0;
  QWC_TRUE(untouched, "emit.bounds: buffer unchanged when too small");
  QWC_EQ_I(fen_emit(&p, NULL, FEN_BUF_SIZE), 0, "emit.bounds: NULL out -> 0");
  QWC_EQ_I(fen_emit(&p, big, 0), 0, "emit.bounds: zero size -> 0");
  return qwc_end();
}

int main(void) {
  int fail = 0;
  fail |= t_roundtrip();
  fail |= t_reject();
  fail |= t_ep();
  fail |= t_emit_bounds();
  return fail ? 1 : 0;
}
