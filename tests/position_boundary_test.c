/* Regressions from the source audit. Expectations are fixed representation
 * contracts, independent of the implementation's limits or validation result. */
#include "position/fen.h"
#include "core/square.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>

static int logical_equal(const Position *a, const Position *b) {
  return memcmp(a->mailbox, b->mailbox, sizeof a->mailbox) == 0 &&
    a->side == b->side && a->cr == b->cr && a->ep_sq == b->ep_sq &&
    a->halfmove == b->halfmove && a->fullmove == b->fullmove &&
    memcmp(a->byPiece, b->byPiece, sizeof a->byPiece) == 0 &&
    memcmp(a->byColor, b->byColor, sizeof a->byColor) == 0 &&
    a->occ == b->occ && a->wKingSq == b->wKingSq &&
    a->bKingSq == b->bKingSq && a->key == b->key;
}

static int t_counters(void) {
  qwc_begin("boundary.counters");
  const int values[] = {0, 99, 100, 101, 149, 150, 255, 256, 65535};
  for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
    char fen[FEN_BUF_SIZE], out[FEN_BUF_SIZE];
    Position p, again;
    const char *why = NULL;
    (void)snprintf(fen, sizeof fen,
                   "7k/8/8/8/8/8/8/KR6 w - - %d 65535", values[i]);
    int ok = fen_load(&p, fen, &why);
    QWC_TRUE(ok, "representable clock loads, including above draw thresholds");
    if (!ok) continue;
    QWC_EQ_I(p.halfmove, values[i], "halfmove preserved");
    QWC_EQ_I(p.fullmove, 65535, "fullmove maximum preserved");
    ok = fen_emit(&p, out, (int)sizeof out);
    QWC_TRUE(ok, "counter boundary emits");
    if (!ok) continue;
    QWC_TRUE(strcmp(fen, out) == 0, "counter text round-trips");
    ok = fen_load(&again, out, NULL);
    QWC_TRUE(ok, "counter boundary reloads without a diagnostic pointer");
    if (ok) QWC_TRUE(logical_equal(&p, &again), "all logical fields restored");
  }
  const char *bad[] = {
    "7k/8/8/8/8/8/8/KR6 w - - 65536 65535",
    "7k/8/8/8/8/8/8/KR6 w - - 99999999999999999999 65535",
    "7k/8/8/8/8/8/8/KR6 w - - 0 65536"
  };
  Position sentinel;
  pos_set_startpos(&sentinel);
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    Position p = sentinel;
    const char *why = NULL;
    QWC_TRUE(!fen_load(&p, bad[i], &why), "overflow rejected before narrowing");
    QWC_TRUE(why && *why, "overflow has a reason");
    QWC_TRUE(logical_equal(&p, &sentinel), "failed load leaves all fields unchanged");
  }
  return qwc_end();
}

static int t_piece_codes(void) {
  qwc_begin("boundary.piece_codes");
  for (unsigned code = 1; code <= 255; code++) {
    /* Numeric ranges are frozen by types.h, not inferred from the validator. */
    if ((code >= 1 && code <= 6) || (code >= 9 && code <= 14)) continue;
    Position p;
    pos_reset(&p);
    p.mailbox[0] = W_KING;
    p.mailbox[63] = B_KING;
    p.mailbox[17] = (u8)code;
    pos_rebuild(&p); /* must stay within arrays even for 16..255 */
    Position before = p;
    const char *why = NULL;
    QWC_TRUE(!pos_validate(&p, &why), "undefined chess piece code rejected");
    QWC_TRUE(why && *why, "invalid piece has a reason");
    QWC_TRUE(logical_equal(&p, &before), "validator does not repair invalid input");
    char out[FEN_BUF_SIZE], untouched[FEN_BUF_SIZE];
    memset(out, 'X', sizeof out);
    memcpy(untouched, out, sizeof out);
    QWC_TRUE(!fen_emit(&p, out, (int)sizeof out), "invalid board is not emitted");
    QWC_TRUE(memcmp(out, untouched, sizeof out) == 0, "failed emit writes no bytes");
  }
  Position p;
  pos_set_startpos(&p);
  p.byPiece[0] = 1;
  QWC_TRUE(!pos_validate(&p, NULL), "unused empty-piece cache must remain zero");
  return qwc_end();
}

static int t_ep_metadata(void) {
  qwc_begin("boundary.ep_metadata");
  Position base;
  int ok = fen_load(&base, "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 20", NULL);
  QWC_TRUE(ok, "fixed legal EP baseline loads");
  if (!ok) return qwc_end();
  QWC_TRUE(pos_ep_capture_is_safe(&base, 36), "baseline e5d6 is safe");
  for (int i = 0; i < 9; i++) {
    Position p = base;
    switch (i) {
      case 0: p.mailbox[35] = NO_PIECE; break; /* captured pawn missing */
      case 1: p.mailbox[35] = B_KNIGHT; break; /* captured piece not a pawn */
      case 2: p.mailbox[43] = W_KNIGHT; break; /* target not empty */
      case 3: p.mailbox[51] = B_KNIGHT; break; /* double-push origin occupied */
      case 4: p.halfmove = 1; break;
      case 5: p.ep_sq = 35; break;            /* wrong target rank */
      case 6: p.side = 2; break;
      case 7: p.ep_sq = 65; break;
      case 8: p.ep_sq = 255; break;
    }
    Position before = p;
    QWC_TRUE(!pos_ep_is_valid_meta(&p), "malformed EP metadata rejected");
    QWC_TRUE(!pos_ep_capture_is_safe(&p, 36), "no per-captor success on bad metadata");
    QWC_TRUE(!pos_ep_is_legal(&p), "no legal EP on bad metadata");
    QWC_EQ_I(pos_canon_ep_file(&p), -1, "bad metadata adds no EP key component");
    QWC_TRUE(logical_equal(&p, &before), "EP queries are read-only");
  }
  base.ep_sq = NO_SQUARE;
  QWC_TRUE(pos_ep_is_valid_meta(&base), "absent EP is valid metadata");
  QWC_TRUE(!pos_ep_capture_is_safe(&base, 36), "absent EP is not a capture");
  QWC_TRUE(!pos_ep_capture_is_safe(&base, 64), "sentinel captor rejected");
  QWC_TRUE(!pos_ep_capture_is_safe(&base, 255), "out-of-range captor rejected");
  return qwc_end();
}

static int t_emit_capacity(void) {
  qwc_begin("boundary.emit_capacity");
  Position p;
  pos_set_startpos(&p);
  const char *expected = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  int n = (int)strlen(expected);
  char out[FEN_BUF_SIZE], untouched[FEN_BUF_SIZE];
  memset(out, 'X', sizeof out);
  memcpy(untouched, out, sizeof out);
  QWC_TRUE(!fen_emit(&p, out, n), "capacity without room for NUL fails");
  QWC_TRUE(memcmp(out, untouched, sizeof out) == 0, "capacity failure is atomic");
  QWC_TRUE(fen_emit(&p, out, n + 1), "exact capacity including NUL succeeds");
  QWC_TRUE(strcmp(out, expected) == 0, "exact-capacity text is complete");
  QWC_EQ_I(out[n + 1], 'X', "byte beyond declared capacity unchanged");
  return qwc_end();
}

int main(void) {
  int fail = t_counters();
  fail |= t_piece_codes();
  fail |= t_ep_metadata();
  fail |= t_emit_capacity();
  return fail ? 1 : 0;
}
