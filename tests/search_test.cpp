/* QwenChess: the iterative-deepening search (focused unit tests).
 *
 * Uses the real FEN loader, the complete legal generator, and the reversible make/
 * unmake. Checks the no-legal-move outcomes (checkmate -> a root-relative mated
 * score, stalemate -> 0), the 50-move and threefold-repetition draw policy (score 0
 * but a legal move still returned), a mate-in-1, an immediate stop before depth 1
 * (a legal fallback move, never 0000, and `interrupted`), and a node-budget-limited
 * search (a valid, legal result, not a partial one). Keeps assertions active in
 * release.
 */
#include <cstdio>
#include <atomic>
#include <array>

#include "core/square.hpp"
#include "movegen/movegen.hpp"
#include "position/fen.hpp"
#include "position/position.hpp"
#include "search/search.hpp"

#include "test_util.hpp"

using namespace qwc;

/* True if `m` is in the complete legal set of `pos`. */
static bool is_legal(const Position& pos, Move m) {
  std::array<Move, kSearchMoveCapacity> moves{};
  std::size_t n = 0;
  if (!movegen_legal(pos, moves, n))
    return false;
  for (std::size_t i = 0; i < n; ++i)
    if (moves[i] == m)
      return true;
  return false;
}

/* A generous-but-FAST limit for the sparse/unit fixtures. Depth 4 is enough to exercise
 * the iterative deepening + the full legal move set while keeping the node count (and
 * thus the unit-test runtime) small. (A full-strength / NPS search is not what this
 * focused unit test is for.) */
static SearchLimits deep() {
  SearchLimits l;
  l.depth = 4;
  return l;
}

int main() {
  qwc_begin("search");
  std::atomic<bool> stop(false);
  Position pos{};
  std::array<StateInfo, 8> states{};

  /* ---- mate in 1: a protected queen mate (a mated score; a legal best move) ---------
   * (Several queen moves mate here; the deterministic generator returns the first. We
   * assert a mated score + a legal move, not a specific square.) */
  QWC_TRUE(fen_load(pos, "k1K5/8/1Q6/8/8/8/8/8 w - - 0 1"), "mate-in-1 FEN loads");
  SearchResult r = do_search(pos, deep(), stop);
  QWC_TRUE(is_legal(pos, r.bestmove), "mate-in-1 bestmove is legal");
  QWC_TRUE(r.score >= VALUE_MATE_IN_MAX_PLY, "mate-in-1 has a positive mated score");
  QWC_TRUE(r.pv_size >= 1, "mate-in-1 has a non-empty PV");

  /* ---- stalemate at the root: 0000 + a draw score of 0 ----------------------------- */
  QWC_TRUE(fen_load(pos, "4k3/8/3Q2K1/8/8/8/8/8 b - - 0 1"), "stalemate FEN loads");
  r = do_search(pos, deep(), stop);
  QWC_EQ_I(r.bestmove, move_none(), "stalemate -> 0000 no-move sentinel");
  QWC_EQ_I(r.score, VALUE_ZERO, "stalemate -> 0");
  QWC_EQ_I(r.completed_depth, 0, "stalemate completes at depth 0");

  /* ---- checkmate at the root: 0000 + a negative mated score ------------------------ */
  QWC_TRUE(fen_load(pos, "kQK5/8/8/8/8/8/8/8 b - - 0 1"), "checkmate FEN loads");
  r = do_search(pos, deep(), stop);
  QWC_EQ_I(r.bestmove, move_none(), "checkmate -> 0000");
  QWC_TRUE(r.score <= VALUE_MATED_IN_MAX_PLY, "checkmate -> a negative mated score");
  QWC_EQ_I(r.completed_depth, 0, "checkmate completes at depth 0");

  /* ---- 50-move draw: legal moves exist, so a legal move is returned, score 0 -------- */
  QWC_TRUE(fen_load(pos, "4k3/8/8/8/8/8/3R4/4K3 w - - 100 1"), "50-move FEN loads");
  r = do_search(pos, deep(), stop);
  QWC_TRUE(is_legal(pos, r.bestmove), "50-move: a legal move is returned (not 0000)");
  QWC_EQ_I(r.bestmove != move_none() ? 1 : 0, 1, "50-move: the move is not the sentinel");
  QWC_EQ_I(r.score, VALUE_ZERO, "50-move clock >=100 -> a draw (0)");

  /* ---- threefold repetition: a legal move is returned, score 0 ---------------------- */
  const char *startpos = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  QWC_TRUE(fen_load(pos, startpos), "startpos loads");
  const Move seq[8] = {
    make_move(square_of(6, 0), square_of(5, 2), MoveFlag::NO_FLAGS, 0),   /* Nf3 */
    make_move(square_of(6, 7), square_of(5, 5), MoveFlag::NO_FLAGS, 0),   /* ...Nf6 */
    make_move(square_of(5, 2), square_of(6, 0), MoveFlag::NO_FLAGS, 0),   /* Ng1 */
    make_move(square_of(5, 5), square_of(6, 7), MoveFlag::NO_FLAGS, 0),   /* ...Ng8 */
    make_move(square_of(6, 0), square_of(5, 2), MoveFlag::NO_FLAGS, 0),   /* Nf3 */
    make_move(square_of(6, 7), square_of(5, 5), MoveFlag::NO_FLAGS, 0),   /* ...Nf6 */
    make_move(square_of(5, 2), square_of(6, 0), MoveFlag::NO_FLAGS, 0),   /* Ng1 */
    make_move(square_of(5, 5), square_of(6, 7), MoveFlag::NO_FLAGS, 0),   /* ...Ng8 */
  };
  for (int i = 0; i < 8; ++i)
    QWC_TRUE(pos_make_move(&pos, &states[i], seq[i]), "shuffling move makes");
  QWC_EQ_I(pos_repetition_count(&pos), 3, "the position is a threefold repetition");
  r = do_search(pos, deep(), stop);
  QWC_TRUE(is_legal(pos, r.bestmove), "threefold: a legal move is returned");
  QWC_EQ_I(r.score, VALUE_ZERO, "threefold repetition -> a draw (0)");

  /* ---- an immediate stop before depth 1: a legal fallback, `interrupted` ------------ */
  QWC_TRUE(fen_load(pos, startpos), "startpos reloads");
  stop = true;
  r = do_search(pos, deep(), stop);
  QWC_TRUE(is_legal(pos, r.bestmove), "immediate stop: a legal move is returned");
  QWC_TRUE(r.bestmove != move_none(), "immediate stop: the move is not 0000");
  QWC_TRUE(r.interrupted, "immediate stop: `interrupted` is set");
  stop = false;

  /* ---- a node-budget-limited search: a valid, legal, non-partial result ------------- */
  SearchLimits tiny;
  tiny.depth = 0;            /* no depth cap */
  tiny.max_nodes = 64;       /* small: hit within the first couple of iterations */
  QWC_TRUE(fen_load(pos, startpos), "startpos reloads (budget)");
  r = do_search(pos, tiny, stop);
  QWC_TRUE(r.interrupted, "node budget: `interrupted` is set");
  QWC_TRUE(is_legal(pos, r.bestmove), "node budget: a legal move is returned");
  QWC_TRUE(r.bestmove != move_none(), "node budget: the move is not 0000");
  QWC_TRUE(r.score > VALUE_MATED_IN_MAX_PLY && r.score < VALUE_MATE_IN_MAX_PLY,
           "node budget: the kept score is a real (non-mate) value");

  /* ---- a quiet search on the start position: a legal move, a real score, a PV ------- */
  QWC_TRUE(fen_load(pos, startpos), "startpos reloads (quiet)");
  r = do_search(pos, deep(), stop);
  QWC_TRUE(is_legal(pos, r.bestmove), "quiet: a legal move is returned");
  QWC_TRUE(r.score > VALUE_MATED_IN_MAX_PLY && r.score < VALUE_MATE_IN_MAX_PLY,
           "quiet: the score is a real (non-mate) value");
  QWC_TRUE(r.completed_depth > 0, "quiet: at least one iteration completed");
  QWC_TRUE(r.pv_size >= 1 && r.pv[0] == r.bestmove, "quiet: the PV starts with the best move");
  QWC_TRUE(r.nodes > 0, "quiet: the node count is positive");

  /* ---- quiescence: a hanging queen is seen at the horizon -----------------------------
   * White Q a1 is attacked by the black rook on a8 (undefended) and it is white's turn.
   * A quiet root move (a king step) would let the quiescence play ...Rxa1, so those lines
   * score a lost queen; the search instead prefers the queen's escape. Hence the best move
   * is the queen's (from a1 = square 0) and the score is positive (the queen survives).
   * Without quiescence every quiet move would tie the escape at the same static score and
   * the deterministic tie-break could pick a quiet move -- the quiescence removes that. */
  QWC_TRUE(fen_load(pos, "rk6/8/8/8/8/8/8/Q3K3 w - - 0 1"), "quiescence FEN loads");
  r = do_search(pos, deep(), stop);
  QWC_TRUE(is_legal(pos, r.bestmove), "quiescence: a legal move is returned");
  QWC_EQ_I((int)move_from(r.bestmove), 0, "quiescence: the best move is the queen's escape (from a1)");
  QWC_TRUE(r.score > VALUE_ZERO, "quiescence: the queen survives, so white is up");
  SearchResult r2 = do_search(pos, deep(), stop);
  QWC_EQ_I(r.bestmove, r2.bestmove, "quiescence: deterministic bestmove");
  QWC_EQ_I(r.score, r2.score, "quiescence: deterministic score");

  int failed = qwc_end();
  return failed ? 1 : 0;
}
