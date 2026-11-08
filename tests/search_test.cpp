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

#include <cstring>

#include "core/square.hpp"
#include "engine/uci_codec.hpp"
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

  /* ---- a REAL PV: every emitted PV move replays (parses + makes) from the root ---- */
  QWC_TRUE(fen_load(pos, startpos), "startpos (pv replay)");
  r = do_search(pos, deep(), stop);
  {
    Position rp{};
    QWC_TRUE(fen_load(rp, startpos), "replay root loads");
    std::array<StateInfo, MAX_PLY> rstates{};
    bool replay_ok = true;
    for (int i = 0; i < r.pv_size; ++i) {
      char tok[8];
      Move m = move_none();
      if (uci_move_emit(r.pv[i], tok, sizeof(tok)) <= 0 || uci_move_parse(rp, tok, m) != 1) { replay_ok = false; break; }
      if (!pos_make_move(&rp, &rstates[i], m)) { replay_ok = false; break; }
    }
    QWC_TRUE(r.pv_size >= 1, "quiet: a non-empty PV");
    QWC_TRUE(replay_ok, "every PV move replays as a legal move from the root");
    QWC_TRUE(r.pv[0] == r.bestmove, "the PV starts with the bestmove");
  }

  /* ---- the root window contains every real score: a mate-in-1 is scored EXACTLY --
   * (A classification-threshold window would exclude the extremal mate value; the
   * true full window must report the actual score, not a bound.) The best move must
   * be a genuine mate. */
  QWC_TRUE(fen_load(pos, "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1"), "mate-in-1 FEN loads");
  r = do_search(pos, deep(), stop);
  QWC_EQ_I(r.score, mate_in(1), "mate-in-1 scored exactly (+30999: the window contained it)");
  QWC_TRUE(is_legal(pos, r.bestmove), "mate-in-1: a legal bestmove");
  QWC_EQ_I(r.completed_depth, 1, "the extremal mate (mate_in(1)) terminates the ID loop at depth 1");
  {
    Position mp{};
    std::array<StateInfo, 2> ms{};
    QWC_TRUE(fen_load(mp, "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1"), "mate check root loads");
    QWC_TRUE(pos_make_move(&mp, &ms[0], r.bestmove), "the bestmove makes");
    std::array<Move, kSearchMoveCapacity> mm{};
    std::size_t mn = 0;
    QWC_TRUE(movegen_legal(mp, mm, mn), "legal generation succeeds after the mate");
    QWC_TRUE(mn == 0 && pos_is_in_check(&mp, mp.side), "the bestmove is a checkmate (no evasion, in check)");
  }

  /* ---- a faster mate is preferred among alternatives: the same position that is a
   *    stalemate UNLESS the queen checks Qa5# -- the immediate mate (distance 1) must
   *    beat the longer forced K+Q mate at every depth. */
  QWC_TRUE(fen_load(pos, "k1K5/8/1Q6/8/8/8/8/8 w - - 0 1"), "stalemate-break FEN loads");
  {
    SearchLimits d16;
    d16.depth = 16;
    r = do_search(pos, d16, stop);
    QWC_EQ_I(r.score, mate_in(1), "the immediate Qa5# is preferred over the longer forced mate");
    QWC_EQ_I(move_from(r.bestmove), square_of(1, 5), "the mate is the queen's (from b6)");
    QWC_EQ_I(move_to(r.bestmove), square_of(0, 4), "landing on a5 (Qa5#)");
    QWC_EQ_I(r.completed_depth, 1, "the extremal mate stops the loop at depth 1 (not depth 16)");
  }

  /* ---- the quiescence seam: mate beats the draw clock, even at the horizon --------
   * (black to move; the king h8 is mated by the queen g7, defended by Kf6; the
   * halfmove clock is 100. The no-legal-move outcome MUST be the mate, never the
   * 50-move draw that the old ordering checked first.) */
  {
    QWC_TRUE(fen_load(pos, "7k/6Q1/5K2/8/8/8/8/8 b - - 100 1"), "mated-at-horizon FEN loads");
    std::atomic<bool> s2(false);
    SearchLimits l;
    auto pr = qwc::search_test::qsearch_probe(pos, 4, -VALUE_INFINITE, +VALUE_INFINITE, l, s2);
    QWC_EQ_I(pr.score, mated_in(4), "a mated horizon (halfmove 100) is a MATE, not a draw");
    QWC_EQ_I((long long)pr.nodes, 1, "the horizon node is counted exactly once");
    QWC_TRUE(!pr.stopped && !pr.failed, "a clean horizon: no stop, no failure");
  }
  /* ---- the node-entry contract via the seam: a limit AT ENTRY stops the node ------
   * (a preset stop / an expired deadline / a node budget of 1: one count, a
   * placeholder, never a value; a stopped node is not a failed one) */
  {
    QWC_TRUE(fen_load(pos, startpos), "startpos (node entry)");
    std::atomic<bool> s2(true);   /* a preset stop */
    SearchLimits l;
    auto pr = qwc::search_test::qsearch_probe(pos, 0, -VALUE_INFINITE, +VALUE_INFINITE, l, s2);
    QWC_EQ_I(pr.score, VALUE_NONE, "a pre-set stop yields the placeholder, never a score");
    QWC_EQ_I((long long)pr.nodes, 1, "the entry is still counted exactly once");
    QWC_TRUE(pr.stopped && !pr.failed, "stopped (a cancellation), not failed");
    std::atomic<bool> s3(false);
    SearchLimits l2;
    l2.has_deadline = true;
    l2.deadline = clock_now() - std::chrono::milliseconds(1);   /* already expired */
    pr = qwc::search_test::qsearch_probe(pos, 0, -VALUE_INFINITE, +VALUE_INFINITE, l2, s3);
    QWC_EQ_I(pr.score, VALUE_NONE, "an expired deadline yields the placeholder");
    QWC_TRUE(pr.stopped && !pr.failed, "a deadline stop is a stop, not a failure");
    SearchLimits l3;
    l3.max_nodes = 1;   /* the budget is exhausted by the single entry */
    pr = qwc::search_test::qsearch_probe(pos, 0, -VALUE_INFINITE, +VALUE_INFINITE, l3, s3);
    QWC_EQ_I(pr.score, VALUE_NONE, "a node budget of 1 yields the placeholder");
    QWC_TRUE(pr.stopped && !pr.failed, "a node-budget stop is a stop, not a failure");
  }
  /* ---- the MAX_PLY boundary: a depth-0 node AT the bound stays in storage --------
   * (root + boundary = MAX_PLY_STACK; a node at ply == MAX_PLY is a horizon: counted
   * once, terminal/draw-checked, a bounded stand-pat; it makes NO child, so nothing
   * is written past the end of any per-ply array. Under ASan the old -1 sizing
   * overflowed here.) */
  {
    QWC_TRUE(fen_load(pos, startpos), "startpos (boundary)");
    std::atomic<bool> s(false);
    SearchLimits l;
    auto pr = qwc::search_test::negamax_probe(pos, MAX_PLY, 0, -VALUE_INFINITE, +VALUE_INFINITE, l, s);
    QWC_EQ_I(pr.score, VALUE_ZERO, "startpos at the horizon: a bounded stand-pat (balanced)");
    QWC_EQ_I((long long)pr.nodes, 1, "the boundary node is counted exactly once");
    QWC_TRUE(!pr.stopped && !pr.failed, "the boundary node is clean");
  }

  /* ---- an interrupted search: the last completed result is kept, the position is
   *    byte-identical, the PV is a real line, and a real score is kept -------------- */
  {
    QWC_TRUE(fen_load(pos, startpos), "startpos (interrupted)");
    char fbefore[128] = {}, fafter[128] = {};   /* zero-init: fen_emit leaves the tail bytes untouched */
    QWC_TRUE(fen_emit(pos, std::span<char>(fbefore, sizeof(fbefore))), "emit before");
    SearchLimits budget;
    budget.max_nodes = 64;   /* completes depth 1, stops inside depth 2 */
    r = do_search(pos, budget, stop);
    QWC_TRUE(r.interrupted && !r.failed, "interrupted by the node budget (not an internal failure)");
    QWC_TRUE(r.bestmove != move_none() && is_legal(pos, r.bestmove), "a legal move from the last completed work");
    QWC_TRUE(r.score > VALUE_TB_LOSS_IN_MAX_PLY && r.score < VALUE_TB_WIN_IN_MAX_PLY, "a real (non-mate, non-none) score is kept");
    QWC_TRUE(r.pv_size >= 1 && r.pv[0] == r.bestmove, "the kept PV is a real line starting at the bestmove");
    QWC_TRUE(fen_emit(pos, std::span<char>(fafter, sizeof(fafter))) && memcmp(fbefore, fafter, 128) == 0,
             "do_search leaves the caller's Position byte-identical");
  }
  /* ---- a stop that lands INSIDE quiescence: the pending value is discarded, the
   *    fallback (a legal move) is published, the position is intact ----------------- */
  {
    QWC_TRUE(fen_load(pos, "rk6/8/8/8/8/8/8/Q3K3 w - - 0 1"), "hanging queen (qsearch stop)");
    char fbefore[128] = {}, fafter[128] = {};   /* zero-init: fen_emit leaves the tail bytes untouched */
    QWC_TRUE(fen_emit(pos, std::span<char>(fbefore, sizeof(fbefore))), "emit before");
    SearchLimits qbudget;
    qbudget.max_nodes = 20;   /* small enough to stop inside a child's quiescence */
    r = do_search(pos, qbudget, stop);
    QWC_TRUE(r.interrupted && !r.failed, "a qsearch-interrupted search is interrupted, not failed");
    QWC_TRUE(r.bestmove != move_none() && is_legal(pos, r.bestmove), "the fallback is a legal move (never 0000 with legal moves)");
    QWC_TRUE(r.score > VALUE_TB_LOSS_IN_MAX_PLY && r.score < VALUE_TB_WIN_IN_MAX_PLY, "a real score (the root's static), not a placeholder");
    QWC_TRUE(fen_emit(pos, std::span<char>(fafter, sizeof(fafter))) && memcmp(fbefore, fafter, 128) == 0,
             "the position is intact after an interrupted qsearch");
  }
  /* ---- a high fullmove root: the u16 counter is accepted; the search normalizes its
   *    private copy to 1 and runs normally ------------------------------------------ */
  {
    QWC_TRUE(fen_load(pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 65535"), "a fullmove of 65535 loads");
    SearchLimits l;
    l.depth = 2;
    r = do_search(pos, l, stop);
    QWC_TRUE(r.bestmove != move_none() && is_legal(pos, r.bestmove) && r.completed_depth > 0,
             "a high-fullmove root searches normally");
    QWC_TRUE(r.score > VALUE_TB_LOSS_IN_MAX_PLY && r.score < VALUE_TB_WIN_IN_MAX_PLY, "a real score at the high-fullmove root");
  }

  int failed = qwc_end();
  return failed ? 1 : 0;
}
