/* QwenChess T004 checkpoint-5 test: the production repetition OCCURRENCE query
 * (pos_repetition_count) and the caller-owned, uncapped history storage/reset
 * semantics. This is the second half of the null-move / history checkpoint; the
 * null make/unmake mechanics live in null_makeunmake_test.c.
 *
 * Groups:
 *  1. rep.basic_cycle -- hand-built startpos + the g1f3/g8f6/f3g1/f6g8 cycle;
 *     the count is 1 for the first three moves, 2 at the close, 2,2,2,3 for the
 *     second cycle; undo restores the count.
 *  2. rep.consecutive_nulls -- two back-to-back nulls count 0, undo restores 1.
 *  3. rep.null_boundary -- a single null + a repeating two-move sequence + a
 *     second sequence + a subsequent null + a cycle; verifies a null is a
 *     boundary (a matching position after it does NOT count the pre-null node).
 *  4. rep.storage -- a 260-ply (> MAX_PLY) chain in caller-owned storage, a
 *     FEN reload that resets it, a FAILED load that preserves it, a rebuild that
 *     preserves it, and two coexisting arrays.
 *
 * NOTE on the spec: the task asked to "verify the count returns to 1" after the
 * second cycle's last two moves. For this exact sequence that value is 2, not 1:
 * undoing the two closing moves lands on the position after move 6, whose key
 * equals the position after move 2 (both are startpos + Nf3 + Nf6, white to
 * move), so the reachable history holds it twice. We assert the correct value
 * (2) and additionally undo all the way back to 1 at the root.
 *
 * All checks are always active; the harness reports aggregates.
 */
#include "position/position.h"
#include "position/state.h"
#include "position/fen.h"
#include "core/types.h"
#include "core/square.h"
#include "core/bitboard.h"
#include "test_util.h"
#include <string.h>

static Square sq_of(int f, int r) { return square_of((Square)f, (Square)r); }

/* Apply one move, asserting success (the fixtures are all legal from the
 * startpos). The caller supplies a distinct, already-zeroed StateInfo. */
static void do_move(Position *pos, StateInfo *st, int f, int r, int tf, int tr,
                    int fl, int pr, const char *name) {
  Move m = make_move(sq_of(f, r), sq_of(tf, tr), (u32)fl, (u32)pr);
  QWC_TRUE(pos_make_move(pos, st, m) == 1, name);
}

static void do_null(Position *pos, StateInfo *st, const char *name) {
  QWC_TRUE(pos_make_null_move(pos, st) == 1, name);
}

/* The four-move knight cycle as (from_file, from_rank, to_file, to_rank). It
 * returns the board to the startpos configuration, so its closing key equals
 * the root key. */
static const int cyc[4][4] = {
  { 6, 0, 5, 2 },   /* g1f3 */
  { 6, 7, 5, 5 },   /* g8f6 */
  { 5, 2, 6, 0 },   /* f3g1 */
  { 5, 5, 6, 7 },   /* f6g8 */
};

static int t_basic_cycle(void) {
  qwc_begin("rep.basic_cycle");
  Position pos;
  StateInfo hist[8]; memset(hist, 0, sizeof hist);
  const char *why = NULL;
  QWC_TRUE(fen_load(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &why) == 1, "load startpos");
  QWC_TRUE(pos.history == NULL, "fresh: history is NULL");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "fresh setup counts 1");
  QWC_EQ_I(pos_repetition_count(NULL), 0, "NULL position counts 0");

  /* first cycle: 1, 1, 1, 2 */
  for (int i = 0; i < 4; i++) {
    do_move(&pos, &hist[i], cyc[i][0], cyc[i][1], cyc[i][2], cyc[i][3], 0, 0, "cycle move");
    QWC_EQ_I(pos_repetition_count(&pos), i == 3 ? 2 : 1, "first-cycle count");
  }
  /* second cycle: 2, 2, 2, 3 */
  for (int i = 0; i < 4; i++) {
    do_move(&pos, &hist[4 + i], cyc[i][0], cyc[i][1], cyc[i][2], cyc[i][3], 0, 0, "cycle move");
    QWC_EQ_I(pos_repetition_count(&pos), i == 3 ? 3 : 2, "second-cycle count");
  }
  QWC_EQ_I(pos_repetition_count(&pos), 3, "after two full cycles counts 3");

  /* undo the two closing moves -> the position after move 6. Its key equals the
   * position after move 2 (startpos + Nf3 + Nf6, white to move), which is still
   * in the reachable history, so the correct count is 2 (the task's "1" would
   * only hold if move 2's twin had also been undone). */
  pos_unmake_move(&pos, &hist[7]);
  pos_unmake_move(&pos, &hist[6]);
  QWC_EQ_I(pos_repetition_count(&pos), 2, "undo the two closing moves -> 2");

  /* undo the rest all the way to the root -> 1 */
  for (int i = 5; i >= 0; i--)
    pos_unmake_move(&pos, &hist[i]);
  QWC_TRUE(pos.history == NULL, "fully unwound: history is NULL");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "back at the root counts 1");
  return qwc_end();
}

static int t_consecutive_nulls(void) {
  qwc_begin("rep.consecutive_nulls");
  Position pos;
  StateInfo hist[8]; memset(hist, 0, sizeof hist);
  const char *why = NULL;
  QWC_TRUE(fen_load(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &why) == 1, "load");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "fresh counts 1");
  do_null(&pos, &hist[0], "null #1");
  QWC_EQ_I(pos_repetition_count(&pos), 0, "a null-produced node counts 0");
  do_null(&pos, &hist[1], "null #2");
  QWC_EQ_I(pos_repetition_count(&pos), 0, "two consecutive nulls count 0");
  pos_unmake_move(&pos, &hist[1]);
  pos_unmake_move(&pos, &hist[0]);
  QWC_TRUE(pos.history == NULL, "undo both nulls -> history NULL");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "undo restores 1");
  return qwc_end();
}

/* A single null + a repeating two-move sequence + a second identical sequence +
 * a subsequent null + a normal cycle. The nulls are the repetition boundary:
 * positions that match a PRE-null node are not counted, and a null node itself
 * counts 0. */
static int t_null_boundary(void) {
  qwc_begin("rep.null_boundary");
  Position pos;
  StateInfo hist[24]; memset(hist, 0, sizeof hist);
  const char *why = NULL;
  QWC_TRUE(fen_load(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &why) == 1, "load");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "fresh counts 1");

  /* single null (a null node) */
  do_null(&pos, &hist[0], "null #1");
  QWC_EQ_I(pos_repetition_count(&pos), 0, "the single null counts 0");

  /* seq1: black g8f6, white g1f3 -> a unique real node in the segment -> 1 */
  do_move(&pos, &hist[1], 6, 7, 5, 5, 0, 0, "seq1 b");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "seq1 move 1 -> 1");
  do_move(&pos, &hist[2], 6, 0, 5, 2, 0, 0, "seq1 w");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "seq1 move 2 (two matching moves) -> 1");

  /* the knights return home (a real node that matches the pre-null root is NOT
   * counted, because the null is the boundary) -> 1 */
  do_move(&pos, &hist[3], 5, 5, 6, 7, 0, 0, "home b");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "return home -> 1 (pre-null root excluded)");
  do_move(&pos, &hist[4], 5, 2, 6, 0, 0, 0, "home w");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "return home (both) -> 1");

  /* seq2: the same two moves again; the result now matches seq1's result -> 2 */
  do_move(&pos, &hist[5], 6, 7, 5, 5, 0, 0, "seq2 b");
  QWC_EQ_I(pos_repetition_count(&pos), 2, "seq2 move 1 -> 2");
  do_move(&pos, &hist[6], 6, 0, 5, 2, 0, 0, "seq2 w");
  QWC_EQ_I(pos_repetition_count(&pos), 2, "second identical two-move sequence -> 2");

  /* a subsequent null (a null node) -> 0 */
  do_null(&pos, &hist[7], "null #2");
  QWC_EQ_I(pos_repetition_count(&pos), 0, "the subsequent null counts 0");

  /* a normal four-move cycle after the null; the null is the boundary, so the
   * final (a repeat of the seq state) counts only within the new segment -> 1 */
  do_move(&pos, &hist[8],  5, 2, 6, 0, 0, 0, "cycle w1");   /* f3g1 */
  QWC_EQ_I(pos_repetition_count(&pos), 1, "cycle move 1 -> 1");
  do_move(&pos, &hist[9],  5, 5, 6, 7, 0, 0, "cycle b1");   /* f6g8 */
  QWC_EQ_I(pos_repetition_count(&pos), 1, "cycle move 2 -> 1");
  do_move(&pos, &hist[10], 6, 0, 5, 2, 0, 0, "cycle w2");   /* g1f3 */
  QWC_EQ_I(pos_repetition_count(&pos), 1, "cycle move 3 -> 1");
  do_move(&pos, &hist[11], 6, 7, 5, 5, 0, 0, "cycle b2");   /* g8f6 */
  QWC_EQ_I(pos_repetition_count(&pos), 1, "normal cycle after the null -> 1");

  /* undo everything (the 12 records) back to the root -> 1 */
  for (int i = 11; i >= 0; i--)
    pos_unmake_move(&pos, &hist[i]);
  QWC_TRUE(pos.history == NULL, "fully unwound: history NULL");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "after undoing all moves + both nulls -> 1");
  return qwc_end();
}

/* A deep (260-ply, well above MAX_PLY) chain in caller-owned storage, then the
 * setup/reset rules: a successful FEN load starts a fresh history, a FAILED load
 * preserves it, pos_rebuild preserves it, and a separate bounded array can
 * coexist. */
#define CYCLES 65
#define PLIES (CYCLES * 4)   /* 260, > MAX_PLY (256) */
static int t_storage(void) {
  qwc_begin("rep.storage");
  Position pos;
  StateInfo hist[PLIES]; memset(hist, 0, sizeof hist);
  const char *why = NULL;
  QWC_TRUE(fen_load(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &why) == 1, "load");

  /* build a 260-move (65-cycle) chain; the history is NOT capped at MAX_PLY */
  for (int c = 0; c < CYCLES; c++) {
    for (int i = 0; i < 4; i++)
      do_move(&pos, &hist[c * 4 + i], cyc[i][0], cyc[i][1], cyc[i][2], cyc[i][3], 0, 0, "deep move");
  }
  /* the head of the chain is the last record; the first record links to NULL */
  QWC_TRUE(pos.history == &hist[PLIES - 1], "head is the last record");
  {
    StateInfo *h = pos.history; int links = 0;
    while (h != NULL) { links++; h = h->prev; }
    QWC_EQ_I(links, PLIES, "the chain holds all 260 records (> MAX_PLY)");
  }
  /* after 65 closed cycles the root position occurred 66 times (65 + the start) */
  QWC_EQ_I(pos_repetition_count(&pos), CYCLES + 1, "deep history: 66 occurrences");

  /* pos_rebuild must preserve the history pointer (it only touches the caches) */
  StateInfo *head_before = pos.history;
  pos_rebuild(&pos);
  QWC_TRUE(pos.history == head_before, "pos_rebuild preserves the history");

  /* a FAILED FEN load preserves the history (and the board) */
  QWC_TRUE(fen_load(&pos, "not a fen at all", &why) == 0, "an invalid FEN is rejected");
  QWC_TRUE(pos.history == head_before, "a failed load preserves the history");
  QWC_EQ_I(pos_repetition_count(&pos), CYCLES + 1, "count unchanged after a failed load");

  /* a SUCCESSFUL FEN load starts a fresh history (the old chain is abandoned,
   * the caller still owns its storage) */
  QWC_TRUE(fen_load(&pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &why) == 1, "a valid FEN loads");
  QWC_TRUE(pos.history == NULL, "a successful load resets the history to NULL");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "the fresh setup counts 1");

  /* the abandoned old chain (still alive in `hist`) and a distinct bounded
   * record can coexist: the loaded position's move links into the new record,
   * not into the abandoned deep array. */
  StateInfo bound; memset(&bound, 0, sizeof bound);
  do_move(&pos, &bound, 6, 0, 5, 2, 0, 0, "Nf3 into the loaded position");
  QWC_TRUE(pos.history == &bound, "the loaded position's history points at the new record");
  QWC_TRUE(pos.history != &hist[0], "it does not point into the abandoned deep array");
  return qwc_end();
}

int main(void) {
  int fail = 0;
  fail |= t_basic_cycle();
  fail |= t_consecutive_nulls();
  fail |= t_null_boundary();
  fail |= t_storage();
  return fail ? 1 : 0;
}
