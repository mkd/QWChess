/* QWChess test: the frozen score-scale contract (independent pin).
 *
 * These checks pin the QwenChess search-constant scheme as ABSOLUTE values (not
 * only relationships between constants), so a silent renumbering of the scale is a
 * build-time / test-time failure. The scheme (see core/types.hpp): real mate scores
 * occupy the closed interval [±30744, ±31000]; the reserved tablebase interval
 * [±30487, ±30743] sits strictly below it (disjoint); the sentinels 32000
 * (INFINITE) and 32001 (NONE) sit above; VALUE_NONE is distinct from VALUE_DRAW.
 * The upstream Stockfish-19 scheme is a different number and is only an upstream
 * reference fact (docs/NNUE_COMPATIBILITY.md §8), not this contract.
 *
 * Self-contained: it links only the test object (core/types.hpp is header-only).
 */
#include <cstdio>
#include "core/types.hpp"
#include "test_util.hpp"

using namespace qwc;

int main() {
  qwc_begin("score-scale");

  /* ---- the absolute values (independent pin of the restored scheme) ---- */
  QWC_EQ_I(VALUE_ZERO, 0, "VALUE_ZERO");
  QWC_EQ_I(VALUE_DRAW, 0, "VALUE_DRAW");
  QWC_EQ_I(VALUE_MATE, 31000, "VALUE_MATE");
  QWC_EQ_I(VALUE_MATE_IN_MAX_PLY, 30744, "VALUE_MATE_IN_MAX_PLY (31000 - MAX_PLY=256)");
  QWC_EQ_I(VALUE_MATED_IN_MAX_PLY, -30744, "VALUE_MATED_IN_MAX_PLY");
  QWC_EQ_I(VALUE_TB, 30743, "VALUE_TB");
  QWC_EQ_I(VALUE_TB_WIN_IN_MAX_PLY, 30487, "VALUE_TB_WIN_IN_MAX_PLY (30743 - 256)");
  QWC_EQ_I(VALUE_TB_LOSS_IN_MAX_PLY, -30487, "VALUE_TB_LOSS_IN_MAX_PLY");
  QWC_EQ_I(VALUE_INFINITE, 32000, "VALUE_INFINITE");
  QWC_EQ_I(VALUE_NONE, 32001, "VALUE_NONE (distinct from 0)");
  QWC_EQ_I(MAX_PLY, 256, "MAX_PLY");

  /* ---- ordering (low..high); the TB and mate intervals must be DISJOINT ---- */
  QWC_TRUE(VALUE_MATED_IN_MAX_PLY < VALUE_TB_LOSS_IN_MAX_PLY, "mated_in_max < tb_loss_in_max");
  QWC_TRUE(VALUE_TB_LOSS_IN_MAX_PLY < VALUE_ZERO, "tb_loss_in_max < 0");
  QWC_TRUE(VALUE_ZERO < VALUE_TB_WIN_IN_MAX_PLY, "0 < tb_win_in_max");
  QWC_TRUE(VALUE_TB_WIN_IN_MAX_PLY < VALUE_TB, "tb_win_in_max < tb");
  QWC_TRUE(VALUE_TB < VALUE_MATE_IN_MAX_PLY, "tb < mate_in_max (disjoint intervals)");
  QWC_TRUE(VALUE_MATE_IN_MAX_PLY < VALUE_MATE, "mate_in_max < mate");
  QWC_TRUE(VALUE_MATE < VALUE_INFINITE, "mate < infinite");
  QWC_TRUE(VALUE_INFINITE < VALUE_NONE, "infinite < none");
  QWC_TRUE(VALUE_TB_LOSS_IN_MAX_PLY == -VALUE_TB_WIN_IN_MAX_PLY, "symmetric TB interval");
  QWC_TRUE(VALUE_MATED_IN_MAX_PLY == -VALUE_MATE_IN_MAX_PLY, "symmetric mate interval");

  /* ---- the mate mapping: plain, unclamped, defined for 0..MAX_PLY ---- */
  QWC_EQ_I(mate_in(0), 31000, "mate_in(0)");
  QWC_EQ_I(mated_in(0), -31000, "mated_in(0)");
  QWC_EQ_I(mate_in(1), 30999, "mate_in(1)");
  QWC_EQ_I(mated_in(2), -30998, "mated_in(2)");
  QWC_EQ_I(mate_in(3), 30997, "mate_in(3)");
  QWC_EQ_I(mate_in(255), 30745, "mate_in(255)");
  QWC_EQ_I(mate_in(256), 30744, "mate_in(256) == the interval floor");
  QWC_EQ_I(mated_in(256), -30744, "mated_in(256) == the interval floor");

  /* ---- classification: a real mate score is exactly the closed interval of
   * either sign; TB values, sentinels, zero and static scores are NOT mate ---- */
  QWC_TRUE(is_mate(31000) && is_mate(30744) && is_mate(30999), "positive mate interval");
  /* is_mate is one-sided (positive only) by design; the two-sided check is is_mate_score. */
  QWC_TRUE(!is_mate(-31000) && !is_mate(-30744) && !is_mate(-30998), "is_mate is positive-side only");
  QWC_TRUE(is_mate_score(31000) && is_mate_score(-31000) && is_mate_score(30744)
           && is_mate_score(-30744), "is_mate_score on both signs");
  QWC_TRUE(!is_mate(30743) && !is_mate(30487), "TB values are NOT mate (30743 / 30487)");
  QWC_TRUE(!is_mate(-30743) && !is_mate(-30487), "TB mirrors are NOT mate");
  QWC_TRUE(!is_mate(32000) && !is_mate(-32000), "INFINITE is NOT mate");
  QWC_TRUE(!is_mate(32001) && !is_mate(-32001), "NONE is NOT mate");
  QWC_TRUE(!is_mate(0), "zero (draw) is NOT mate");
  QWC_TRUE(!is_mate(13500) && !is_mate(-13500), "a static score is NOT mate");
  QWC_TRUE(!is_mate_score(30743) && !is_mate_score(-30487) && !is_mate_score(32001)
           && !is_mate_score(0), "is_mate_score excludes non-mate values");

  /* ---- the signed mate distance (defined for classified mate scores; 0 otherwise) ---- */
  QWC_EQ_I(mate_distance(mate_in(1)), 1, "mate_distance(mate_in(1))");
  QWC_EQ_I(mate_distance(mate_in(3)), 3, "mate_distance(mate_in(3))");
  QWC_EQ_I(mate_distance(mate_in(256)), 256, "mate_distance(mate_in(256))");
  QWC_EQ_I(mate_distance(mated_in(2)), -2, "mate_distance(mated_in(2))");
  QWC_EQ_I(mate_distance(mated_in(4)), -4, "mate_distance(mated_in(4))");
  QWC_EQ_I(mate_distance(mated_in(256)), -256, "mate_distance(mated_in(256))");
  QWC_EQ_I(mate_distance(mated_in(0)), 0, "mated at the root is 0");
  QWC_EQ_I(mate_distance(0), 0, "distance of a draw is 0");
  QWC_EQ_I(mate_distance(VALUE_TB), 0, "distance of a TB value is 0");
  QWC_EQ_I(mate_distance(VALUE_INFINITE), 0, "distance of INFINITE is 0");
  QWC_EQ_I(mate_distance(VALUE_NONE), 0, "distance of NONE is 0");

  /* The material static-eval bound must stay strictly below the reserved interval. */
  QWC_TRUE(13500 < VALUE_TB_WIN_IN_MAX_PLY, "material bound < tb_win_in_max");
  QWC_TRUE(-13500 > VALUE_TB_LOSS_IN_MAX_PLY, "-material bound > tb_loss_in_max");

  return qwc_end() ? 1 : 0;
}
