/* QwenChess: the temporary Material evaluation (focused unit tests).
 *
 * Verifies the labelled-TMPORARY static material balance: the fixed piece values,
 * the side-to-move sign, a promoted piece being valued as that piece, the start
 * position being 0, and that the evaluation is a pure read (it leaves the Position,
 * its history and its records byte-for-byte unchanged). Assertions stay active in
 * release (test_util is never NDEBUG-guarded).
 */
#include <cstdio>
#include <cstring>

#include "core/square.hpp"
#include "position/fen.hpp"
#include "position/position.hpp"
#include "search/material.hpp"

#include "test_util.hpp"

using namespace qwc;

int main() {
  qwc_begin("material");

  Position pos{};
  const char *startpos   = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  const char *startpos_b = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1";

  /* 1. The orthodox start position has equal material -> 0 (for either side). */
  QWC_TRUE(fen_load(pos, startpos), "startpos (w) loads");
  QWC_EQ_I(eval_material(pos), 0, "startpos (w) == 0");
  QWC_TRUE(fen_load(pos, startpos_b), "startpos (b) loads");
  QWC_EQ_I(eval_material(pos), 0, "startpos (b) == 0");

  /* 2. A material imbalance: black missing its queen (d8) and h8 rook -> white +1400
   *    in the white perspective (a full white army minus a black army lacking Q+R). */
  const char *white_extra = "rnb1kbn1/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQ - 0 1";
  QWC_TRUE(fen_load(pos, white_extra), "imbalance (w) loads");
  QWC_EQ_I(eval_material(pos), 1400, "white extra Q+R == +1400 (white to move)");

  /* 3. Side-to-move sign: the same board, black to move, is the exact negation. */
  const char *white_extra_b = "rnb1kbn1/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQ - 0 1";
  QWC_TRUE(fen_load(pos, white_extra_b), "imbalance (b) loads");
  QWC_EQ_I(eval_material(pos), -1400, "same board, black to move == -1400");

  /* 4. A promoted queen on the back rank is valued as a queen (900), not a pawn. The
   *    black king sits on c2 (off the queen's a-file, rank-8 and a8-h1 diagonal), so
   *    the FEN is legal. */
  const char *promo_q  = "Q7/8/8/8/8/8/2k5/4K3 w - - 0 1";
  const char *promo_qb = "8/8/8/8/8/8/q7/4K2k w - - 0 1";
  QWC_TRUE(fen_load(pos, promo_q), "white promoted Q loads");
  QWC_EQ_I(eval_material(pos), 900, "white Q on a8 == +900");
  QWC_TRUE(fen_load(pos, promo_qb), "black queen loads");
  QWC_EQ_I(eval_material(pos), -900, "black Q on a7 == -900");

  /* 4b. The 15-non-king-piece bound: a promoted queen added to a nearly-full army
   *     leaves 15 non-king white pieces (7 P + 2 R + 2 N + 2 B + 2 Q) = 4800 cp,
   *     still strictly inside the reserved TB/mate interval (kMaterialMax is 15*900). */
  const char *promo_many = "7k/8/8/8/8/4Q3/PPPP1PPP/RNBQKBNR w KQ - 0 1";
  QWC_TRUE(fen_load(pos, promo_many), "15 non-king white pieces loads");
  QWC_EQ_I(eval_material(pos), 4800, "15 non-king pieces == +4800 (within kMaterialMax)");
  QWC_TRUE(eval_material(pos) > VALUE_TB_LOSS_IN_MAX_PLY && eval_material(pos) < VALUE_TB_WIN_IN_MAX_PLY,
           "material stays strictly inside the reserved TB/mate interval");

  /* 5. The evaluation is a pure read over a position with a LIVE history chain: it
   *    leaves the Position valid, the history pointer intact, and every byte unchanged.
   *    Build the chain with one real make, snapshot, evaluate, and diff. */
  Position h{};
  std::array<StateInfo, 8> states{};
  QWC_TRUE(fen_load(h, startpos), "hist root loads (nullptr history)");
  QWC_TRUE(h.history == nullptr, "fresh history is nullptr");
  Move e2e4 = make_move(square_of(4, 1), square_of(4, 3), MoveFlag::NO_FLAGS, 0);   /* e2 -> e4 */
  QWC_TRUE(pos_make_move(&h, &states[0], e2e4), "make e2e4 (live history)");
  QWC_TRUE(h.history == &states[0], "history links the made record");
  Position before = h;      /* whole-Position snapshot (valid: the records are stable) */
  StateInfo *hist_before = h.history;
  QWC_EQ_I(eval_material(h), 0, "hist eval (still balanced) == 0");
  QWC_TRUE(h.history == hist_before, "eval leaves the history pointer unchanged");
  QWC_TRUE(memcmp(&h, &before, sizeof(Position)) == 0, "eval leaves the Position unchanged");
  QWC_TRUE(pos_validate(&h, nullptr), "Position still validates after eval");

  int failed = qwc_end();
  return failed ? 1 : 0;
}
