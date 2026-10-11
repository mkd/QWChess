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
 *  4. rep.storage -- a 260-ply (> MAX_PLY) chain in caller-owned storage; a
 *     distinct search array attached to the still-live head (66 -> 67, back to 66);
 *     a FEN reload that resets it, a FAILED load that preserves it, a rebuild that
 *     preserves it, and two coexisting arrays.
 *  5. rep.null_match_boundary -- a null whose post-null key EQUALS an earlier real
 *     position (the board cycle restores the initial placement and the null flips
 *     the side back) counts 0, never 1 (the defect-2 regression; + mirror).
 *
 * On the intermediate undo in basic_cycle: undoing the two closing moves lands on
 * the position after move 6, whose key equals the position after move 2 (both are
 * startpos + Nf3 + Nf6, white to move), so the reachable history holds it twice and
 * the correct count is 2; the test then undoes all the way back to 1 at the root.
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
   * in the reachable history, so the correct count is 2. (It would be 1 only if
   * move 2's twin had also been undone, which it has not here.) */
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

/* Defect-2 regression: a null whose post-null key EQUALS an earlier real position
 * must count 0, not 1. The five real moves restore the exact initial placement
 * with the side flipped; the null flips the side back, so the current key equals
 * the initial key. The production query must return 0 for the null node and must
 * NOT walk the ancestors (which would count the earlier real occurrence across the
 * very null boundary the rule exists to stop at). Calls the real production query. */
static void run_null_match(const char *fen, const int mv[5][4],
                           const char *exp_final_fen, const char *name) {
  Position pos;
  StateInfo hist[5]; memset(hist, 0, sizeof hist);
  const char *why = NULL;
  QWC_TRUE(fen_load(&pos, fen, &why) == 1, name);
  u64 initial_key = pos.key;
  QWC_TRUE(pos.history == NULL, name);
  QWC_EQ_I(pos_repetition_count(&pos), 1, "fresh setup counts 1");
  for (int i = 0; i < 5; i++)
    do_move(&pos, &hist[i], mv[i][0], mv[i][1], mv[i][2], mv[i][3], 0, 0, "a real transition");
  /* the five moves restore the initial placement with the side flipped */
  QWC_TRUE(pos.key != initial_key, "after five moves the side flipped (key != initial)");
  StateInfo nrec; memset(&nrec, 0, sizeof nrec);
  QWC_TRUE(pos_make_null_move(&pos, &nrec) == 1, "the null is accepted");
  QWC_EQ_U64(pos.key, initial_key, "the null restores the initial key (board + side)");
  QWC_EQ_I(pos_repetition_count(&pos), 0, "a null matching an earlier real position counts 0");
  char fbuf[FEN_BUF_SIZE];
  QWC_TRUE(fen_emit(&pos, fbuf, (int)sizeof fbuf) == 1, "emit the final fen");
  QWC_TRUE(strcmp(fbuf, exp_final_fen) == 0, "the final fen is exact");
  /* full LIFO undo restores the prior head, counters, board, key and count */
  pos_unmake_move(&pos, &nrec);
  for (int i = 4; i >= 0; i--)
    pos_unmake_move(&pos, &hist[i]);
  QWC_TRUE(pos.history == NULL, "fully unwound: history NULL");
  QWC_EQ_U64(pos.key, initial_key, "undo restores the initial key");
  QWC_EQ_I((int)pos.halfmove, 0, "undo restores the halfmove counter");
  QWC_EQ_I((int)pos.fullmove, 1, "undo restores the fullmove counter");
  QWC_EQ_I(pos_repetition_count(&pos), 1, "undo restores the occurrence count to 1");
}

static int t_null_match_boundary(void) {
  qwc_begin("rep.null_match_boundary");
  /* the exact supplied sequence: white king + black rook cycle, then a null */
  static const int orig[5][4] = {
    { 0, 0, 0, 1 },   /* a1a2 */
    { 7, 7, 6, 7 },   /* h8g8 */
    { 0, 1, 1, 0 },   /* a2b1 */
    { 6, 7, 7, 7 },   /* g8h8 */
    { 1, 0, 0, 0 },   /* b1a1 */
  };
  run_null_match("5r1k/8/8/8/8/8/8/K1R5 w - - 0 1", orig,
                 "5r1k/8/8/8/8/8/8/K1R5 w - - 5 3", "exact reproducer (white to move)");
  /* its rank-reflected / color-swapped counterpart */
  static const int refl[5][4] = {
    { 0, 7, 0, 6 },   /* a8a7 */
    { 7, 0, 6, 0 },   /* h1g1 */
    { 0, 6, 1, 7 },   /* a7b8 */
    { 6, 0, 7, 0 },   /* g1h1 */
    { 1, 7, 0, 7 },   /* b8a8 */
  };
  run_null_match("k1r5/8/8/8/8/8/8/5R1K b - - 0 1", refl,
                 "k1r5/8/8/8/8/8/8/5R1K b - - 5 4", "rank-reflect / color-swap mirror");
  return qwc_end();
}

/* A deep (260-ply, well above MAX_PLY) chain in caller-owned storage, then the
 * setup/reset rules: a still-live head with a distinct search array attached, a
 * successful FEN load that resets it, a FAILED load that preserves it, pos_rebuild
 * preserving it, and two coexisting arrays. */
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

  /* Attach a separate (search-style) array to the STILL-LIVE pre-root chain: one
   * knight cycle on a distinct four-record array links its first record to the
   * retained 260-ply head, so the root position now occurs 67 times. Undoing the
   * four records returns the original head and count 66. (This verifies storage
   * and history linkage; it does not claim a game continues after automatic draw
   * adjudication -- that is a search policy, not a position/storage concern.) */
  {
    StateInfo *head260 = pos.history;   /* the 260-ply head (count 66) */
    QWC_TRUE(pos.history == &hist[PLIES - 1], "the head is the last deep record");
    StateInfo search[4]; memset(search, 0, sizeof search);
    for (int i = 0; i < 4; i++)
      do_move(&pos, &search[i], cyc[i][0], cyc[i][1], cyc[i][2], cyc[i][3], 0, 0, "an attached search move");
    QWC_TRUE(pos.history == &search[3], "the attached head is the new last record");
    QWC_TRUE(search[0].prev == head260, "the first new record links to the retained 260-ply head");
    QWC_TRUE(head260 == &hist[PLIES - 1], "the retained head is still the deep record");
    QWC_EQ_I(pos_repetition_count(&pos), CYCLES + 2, "after one attached cycle: 67 occurrences");
    for (int i = 3; i >= 0; i--)
      pos_unmake_move(&pos, &search[i]);
    QWC_TRUE(pos.history == head260, "undo restores the original 260-ply head");
    QWC_TRUE(pos.history == &hist[PLIES - 1], "the restored head is the deep record again");
    QWC_EQ_I(pos_repetition_count(&pos), CYCLES + 1, "undo restores 66 occurrences");
  }

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
  fail |= t_null_match_boundary();
  fail |= t_storage();
  return fail ? 1 : 0;
}
