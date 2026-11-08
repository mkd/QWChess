/* QwenChess: the deterministic single-worker iterative-deepening search (implementation).
 *
 * See search.hpp for the public contract. The hot path is a full-width fail-soft
 * negamax alpha-beta over the complete legal generator, with the temporary Material
 * evaluation at the depth-zero horizon and a quiescence search over the quiet
 * horizon. Every searched node is counted exactly once and polls the limits at
 * entry (the node-entry contract); the depth-0 handoff to the quiescence is a
 * single node. Interruption (a stop / limit) and internal failure (a generation /
 * make error) are separate booleans that are never numeric scores: when either is
 * set, the recursion unwinds (every applied move is unmade) and the pending
 * placeholder value is discarded, never negated or compared as an evaluation.
 */
#include "search/search.hpp"

#include <memory>
#include <span>

#include "movegen/movegen.hpp"
#include "search/material.hpp"

namespace qwc {

/* A conservative per-ply move-storage bound == kSearchMoveCapacity. */
static constexpr int kPlyMoveCap = kSearchMoveCapacity;   /* 436 */

/* Per-ply mutable search data (stable, preallocated). */
struct SearchData {
  Move bestmove = move_none();   /* the best move found at this ply (the PV head) */
};

/* The mutable search state: the node count + the interrupt / failure flags + the
 * limit sources. `stop` is the UCI cancellation flag (polled with relaxed ordering;
 * not owned). `stopped` = a limit / cancellation (a valid result is kept);
 * `failed` = an internal error (distinct; reported honestly, never a terminal). */
struct SearchState {
  long long              nodes        = 0;
  bool                   stopped      = false;
  bool                   failed       = false;
  long long              node_limit   = 0;   /* 0 = no node cap */
  bool                   has_deadline = false;
  TimePoint              deadline     {};
  const std::atomic<bool>* stop       = nullptr;
};

/* All per-search hot-path storage, preallocated ONCE into a single heap block for
 * the whole search. No per-node heap allocation; no whole-Position snapshot per ply.
 * Every per-ply array has MAX_PLY_STACK (= MAX_PLY + 1: root + boundary) entries so
 * the documented bound (a node at ply == MAX_PLY) stays in bounds; a node at the
 * boundary never makes a child, so nothing is written past the end. `moves` is sized
 * for every ply (depth-first: each ply's generated moves must persist while its
 * children are searched). The `pv` table is rectangular (MAX_PLY_STACK rows of
 * MAX_PLY_STACK moves): row `p` is the principal-variation line from ply `p`, and
 * `pv_len` bounds it; a row from ply `p` can hold at most MAX_PLY_STACK - p moves
 * (a line from `p` to the last legal ply). */
struct SearchWork {
  Position                                          pos {};
  std::array<StateInfo, MAX_PLY_STACK>              states {};
  std::array<SearchData, MAX_PLY_STACK>             sd {};
  std::array<Move, MAX_PLY_STACK * kPlyMoveCap>     moves {};
  std::array<Move, MAX_PLY_STACK * MAX_PLY_STACK>   pv {};
  std::array<int, MAX_PLY_STACK>                    pv_len {};
};

static inline Move* pv_row(SearchWork& w, int ply) { return &w.pv[(std::size_t)ply * MAX_PLY_STACK]; }
static inline int   pv_row_cap(int ply) { return MAX_PLY_STACK - ply; }

/* True when a limit / the cancellation flag has been reached (checked after the
 * node count). Sets `st.stopped` (never a score). */
static void check_stop(SearchState& st) {
  if (st.node_limit > 0 && st.nodes >= st.node_limit)
    st.stopped = true;
  if (!st.stopped && st.has_deadline && clock_now() >= st.deadline)
    st.stopped = true;
  if (!st.stopped && st.stop->load(std::memory_order_relaxed))
    st.stopped = true;
}

/* True when the recursion must stop for any reason (a limit / cancellation or an
 * internal failure). A stopped / failed child's numeric result is a placeholder the
 * caller discards, never negated or compared as an evaluation. */
static bool search_over(const SearchState& st) {
  return st.stopped || st.failed;
}

/* The shared node entry (the node-entry contract): honor an already-set stop /
 * failure, count the node exactly once, then poll the limits. Returns false when
 * the node must return a placeholder without searching. */
static bool node_entry(SearchState& st) {
  if (search_over(st))
    return false;
  if (++st.nodes > 0)
    check_stop(st);
  return !search_over(st);
}

/* True iff `m` is a capture in `pos`: an en-passant, or a move onto a square holding an
 * enemy piece. (There is no "capture" move flag; EP carries one, ordinary captures are
 * detected from the destination occupancy.) */
static bool is_capture(const Position& pos, Move m) {
  if (is_ep(m))
    return true;
  Color enemy = (pos.side == Color::WHITE) ? Color::BLACK : Color::WHITE;
  Piece p = pos.mailbox[move_to(m)];
  return p != Piece::NO_PIECE && piece_color(p) == enemy;
}

/* Classify a generated (possibly empty) legal set. A no-legal-move terminal
 * (checkmate / stalemate) precedes the draw policy, which precedes any static
 * evaluation -- this ordering holds at every ply, including the depth/ply horizon
 * (a mated position with a halfmove clock >= 100 is a MATE, not a draw). Returns
 * true and the score when the position is terminal or a draw (search nothing);
 * false when it must be searched. */
static bool terminal_or_draw(const Position& pos, std::size_t n, SearchPly ply, Value& out) {
  if (n == 0) {
    out = pos_is_in_check(&pos, pos.side) ? mated_in(ply) : VALUE_ZERO;   /* mate beats the draw clock */
    return true;
  }
  if (pos.halfmove >= 100 || pos_repetition_count(&pos) >= 3) {
    out = VALUE_ZERO;   /* the draw policy (a search policy, not a FIDE adjudication) */
    return true;
  }
  return false;
}

/* The quiescence search (a fail-soft search over the quiet horizon): in check it
 * searches every legal evasion; otherwise it searches only the captures, with the
 * static score as the stand-pat lower bound (a stand-pat beta cutoff is applied
 * only when not in check). The node-entry contract applies (the node is counted
 * once and the limits are polled at entry and after every child); at the horizon
 * boundary (ply == MAX_PLY) it performs the terminal / draw check and a bounded
 * stand-pat, but no move loop and no child make (the undo stack ends there). */
static Value qsearch(Position& pos, SearchWork& work, SearchState& st,
                     SearchPly ply, Value alpha, Value beta) {
  if (!node_entry(st))
    return VALUE_NONE;
  work.pv_len[ply] = 0;   /* a horizon node contributes no PV (the line ends at the horizon) */

  Move* moves = work.moves.data() + (std::size_t)ply * kPlyMoveCap;   /* ply <= MAX_PLY: in bounds */
  std::span<Move> span(moves, kPlyMoveCap);
  std::size_t n = 0;
  if (!movegen_legal(pos, span, n)) {
    st.failed = true;   /* the position is legal; a generation failure is an internal error */
    return VALUE_NONE;
  }

  Value v;
  if (terminal_or_draw(pos, n, ply, v))   /* mate / stalemate precede the draw policy and the static eval */
    return v;

  if (ply >= MAX_PLY)
    return eval_material(pos);   /* a bounded stand-pat at the horizon (no child beyond the undo stack) */

  bool in_check = pos_is_in_check(&pos, pos.side);
  Value best = in_check ? -VALUE_INFINITE : eval_material(pos);
  if (!in_check) {
    if (best >= beta)
      return best;   /* a fail-high from the stand-pat (never applied while in check) */
    if (best > alpha)
      alpha = best;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (search_over(st))
      break;   /* a stop / limit was reached (a child may have set it): the pending value is discarded */
    Move m = moves[i];
    if (!in_check && !is_capture(pos, m))
      continue;                  /* not in check: only the captures */
    if (!pos_make_move(&pos, &work.states[ply], m)) {   /* a legal move must make */
      st.failed = true;
      break;                           /* do NOT unmake a record that was never applied */
    }
    Value value = -qsearch(pos, work, st, (SearchPly)(ply + 1), -beta, -alpha);
    pos_unmake_move(&pos, &work.states[ply]);   /* always unmake an applied record: balanced */
    if (search_over(st))
      break;   /* the child was interrupted / failed: discard the placeholder */
    if (value > best) {
      best = value;
      if (value > alpha)
        alpha = value;
      if (alpha >= beta)
        break;   /* a fail-high (a cutoff) */
    }
  }
  return best;
}

/* The fail-soft negamax at `ply`, `depth`. `alpha` / `beta` are the side-to-move
 * perspective window. Returns the node's (fail-soft) score, or a placeholder
 * (VALUE_NONE) when the search must stop / failed -- the caller discards it on
 * unwind. A node at the horizon boundary (depth <= 0, or ply >= MAX_PLY) delegates
 * the whole node to the quiescence (one node entry; no per-ply array is written at
 * or past the end of the root + boundary storage, and no child is made at the
 * boundary). */
static Value negamax(Position& pos, SearchWork& work, SearchState& st,
                     SearchPly ply, int depth, Value alpha, Value beta) {
  if (depth <= 0 || ply >= MAX_PLY)
    return qsearch(pos, work, st, ply, alpha, beta);   /* the horizon */

  if (!node_entry(st))
    return VALUE_NONE;

  SearchData& sd = work.sd[ply];      /* ply < MAX_PLY: in bounds */
  sd.bestmove = move_none();
  work.pv_len[ply] = 0;   /* a node starts with an empty line (terminal / draw paths leave it 0) */

  Move* moves = work.moves.data() + (std::size_t)ply * kPlyMoveCap;
  std::span<Move> span(moves, kPlyMoveCap);
  std::size_t n = 0;
  if (!movegen_legal(pos, span, n)) {
    st.failed = true;   /* the position is legal; a generation failure is an internal error */
    return VALUE_NONE;
  }

  Value v;
  if (terminal_or_draw(pos, n, ply, v))   /* mate / stalemate precede the draw policy and the static eval */
    return v;

  /* Search the moves in the current generator order (no new strength heuristics this
   * milestone; ties break by that order, so the search is deterministic). */
  Value best = -VALUE_INFINITE;   /* strictly below the lowest possible real score */
  for (std::size_t i = 0; i < n; ++i) {
    if (search_over(st))
      break;
    Move m = moves[i];
    if (!pos_make_move(&pos, &work.states[ply], m)) {   /* a legal move must make */
      st.failed = true;
      break;   /* do NOT unmake a record that was never applied */
    }
    Value value = -negamax(pos, work, st, (SearchPly)(ply + 1), depth - 1, -beta, -alpha);
    pos_unmake_move(&pos, &work.states[ply]);   /* always unmake an applied record: balanced */
    if (search_over(st))
      break;   /* the child was interrupted / failed: the placeholder is discarded */
    if (value > best) {
      best = value;
      sd.bestmove = m;
      /* A real PV: snapshot [m] + the selected child's continuation (row ply+1) into
       * row ply. Later siblings overwrite row ply+1, so the copy is taken at the moment
       * of the improvement (the child's row is correct then); a row from ply `p` holds
       * at most MAX_PLY_STACK - p moves (a line from `p` to the last legal ply). */
      int clen = work.pv_len[ply + 1];
      if (clen > pv_row_cap((int)ply) - 1)
        clen = pv_row_cap((int)ply) - 1;   /* the bounded row (a defensive cap) */
      Move* row = pv_row(work, (int)ply);
      row[0] = m;
      for (int c = 0; c < clen; ++c)
        row[1 + c] = pv_row(work, (int)ply + 1)[c];
      work.pv_len[ply] = 1 + clen;
      if (value > alpha)
        alpha = value;
      if (alpha >= beta)
        break;   /* a fail-soft beta cutoff (the PV was recorded above) */
    }
  }
  /* On unwind `best` may be a stale/placeholder value; the caller discards it when
   * the search must stop / failed. Otherwise it is the node's fail-soft score. */
  return best;
}

/* The root's static depth-0 score (used as the initial score if no iteration
 * completes): the draw policy, else Material. (The caller has already established
 * the root has legal moves for the fallback path, so the no-legal-move outcomes
 * are decided there.) */
static Value root_static_score(const Position& pos) {
  if (pos.halfmove >= 100 || pos_repetition_count(&pos) >= 3)
    return VALUE_ZERO;
  return eval_material(pos);
}

SearchResult do_search(Position root, const SearchLimits& limits, std::atomic<bool>& stop,
                       const SearchObserver& observer) {
  SearchResult res;
  res.bestmove = move_none();
  res.score = VALUE_NONE;
  res.completed_depth = 0;
  res.pv_size = 0;
  res.nodes = 0;
  res.interrupted = false;
  res.failed = false;

  auto work = std::make_unique<SearchWork>();
  work->pos = root;                    /* the mutable copy (the history pointer is borrowed) */
  work->pos.fullmove = 1;              /* normalize ONLY the private fullmove counter */

  SearchState st;
  st.node_limit = limits.max_nodes;
  st.deadline = limits.deadline;
  st.has_deadline = limits.has_deadline;
  st.stop = &stop;
  st.nodes = 0;
  st.stopped = false;
  st.failed = false;

  /* Generate the root's legal moves. A generation FAILURE is an internal error, never
   * relabelled a terminal: only a GENUINE empty legal set (n == 0) is mate / stalemate. */
  Move* root_moves = work->moves.data();   /* ply 0 */
  std::span<Move> span(root_moves, kPlyMoveCap);
  std::size_t n = 0;
  if (!movegen_legal(work->pos, span, n)) {
    res.failed = true;
    res.nodes = 1;
    return res;   /* 0000 + the error flag; the protocol layer reports it honestly */
  }

  if (n == 0) {
    /* No legal move at the root: checkmate or stalemate -> the 0000 no-move sentinel. */
    res.bestmove = move_none();
    res.score = pos_is_in_check(&work->pos, work->pos.side) ? mated_in(0) : VALUE_ZERO;
    res.completed_depth = 0;
    res.nodes = 1;
    return res;
  }

  /* A legal fallback, selected BEFORE the first (potentially interrupted) iteration:
   * the root always returns a legal move even if a stop arrives before depth 1, or the
   * draw policy fires before the move loop. Saved in a local (the ply-0 buffer is
   * rewritten by the search, though deterministically to the same first move). */
  Move fallback = root_moves[0];
  res.bestmove = fallback;
  res.score = root_static_score(work->pos);   /* used only if no iteration completes */

  /* The search depth bound: an explicit depth is honored as-is (0 = no completed
   * iterations -> the fallback result), capped at the documented MAX_PLY. */
  int max_depth = limits.depth > 0 ? limits.depth : MAX_PLY;
  if (max_depth > MAX_PLY)
    max_depth = MAX_PLY;

  for (int depth = 1; depth <= max_depth; ++depth) {
    if (search_over(st))
      break;
    /* A true full window: every real score (including every mate score near
     * ±VALUE_MATE) is strictly inside [-VALUE_INFINITE, +VALUE_INFINITE]. A
     * classification threshold (VALUE_MATE_IN_MAX_PLY) is NOT a search infinity
     * and must never bound the root. */
    Value s = negamax(work->pos, *work, st, 0, depth,
                      -VALUE_INFINITE, +VALUE_INFINITE);
    if (search_over(st))
      break;   /* this iteration was interrupted (or failed); keep the last fully completed result */
    /* This iteration completed fully: record the move, score and PV. If the draw policy
     * fired at the root before the move loop (leaving bestmove unset), fall back to a
     * legal move: a root with legal moves must return one even when a draw is available. */
    if (work->sd[0].bestmove == move_none())
      work->sd[0].bestmove = fallback;
    res.bestmove = work->sd[0].bestmove;   /* a legal move (the generator's choice) */
    res.score = s;
    res.completed_depth = depth;
    /* Commit the real PV: the ROOT's line is row 0 of the rectangular PV table
     * (contiguous moves pv[0..plen-1] -- NOT the heads of the deeper ply rows, which
     * belong to other nodes). Every emitted move replays in order from the root. */
    res.pv_size = 0;
    int plen = work->pv_len[0];
    if (plen > MAX_PLY)
      plen = MAX_PLY;   /* a root line ends no later than the last legal ply */
    Move* root_row = pv_row(*work, 0);
    for (int p = 0; p < plen; ++p)
      res.pv[res.pv_size++] = root_row[p];
    /* The cumulative node count of the completed result (it includes the quiescence
     * work; the count after an interrupted LAST iteration may be smaller than the
     * final count, never larger). It must be current BEFORE the observer is called,
     * so every completed-iteration report shows the true total, including in the
     * final report of an uninterrupted search (where it equals the result's nodes). */
    res.nodes = st.nodes;
    /* Progress reporting (after the completed result / PV is saved; at most one call
     * per completed iteration, never per node). */
    if (observer.on_iteration)
      observer.on_iteration(res, observer.user);
    /* A safe early termination: only the two extremal mate scores can never improve.
     * A win at ply 1 (mate_in(1): the opponent is mated on the first reply -- the best
     * score a root with a legal move can have) and a loss at ply 2 (mated_in(2): the
     * root has a legal move, so it cannot already be mated at ply 0 -- the worst in the
     * loop) are final. A slower mate CAN improve (a faster mate is a higher score), so
     * no other mate score stops the loop. */
    if (s >= mate_in(1) || s <= mated_in(2))
      break;
  }

  res.interrupted = st.stopped;
  res.failed = st.failed;
  res.nodes = st.nodes;
  return res;
}

/* ---- the white-box test seam (see search.hpp) --------------------------------- */

namespace search_test {

/* Shared probe setup: a fresh arena from `root` + the do_search limit semantics. */
static SearchState probe_state(const SearchLimits& limits, std::atomic<bool>& stop) {
  SearchState st;
  st.node_limit = limits.max_nodes;
  st.deadline = limits.deadline;
  st.has_deadline = limits.has_deadline;
  st.stop = &stop;
  st.nodes = 0;
  st.stopped = false;
  st.failed = false;
  return st;
}

ProbeResult qsearch_probe(Position root, SearchPly ply, Value alpha, Value beta,
                          const SearchLimits& limits, std::atomic<bool>& stop) {
  auto work = std::make_unique<SearchWork>();
  work->pos = root;
  work->pos.fullmove = 1;
  SearchState st = probe_state(limits, stop);
  Value s = qsearch(work->pos, *work, st, ply, alpha, beta);
  ProbeResult pr;
  pr.score = s;
  pr.nodes = st.nodes;
  pr.stopped = st.stopped;
  pr.failed = st.failed;
  return pr;
}

ProbeResult negamax_probe(Position root, int ply, int depth, Value alpha, Value beta,
                          const SearchLimits& limits, std::atomic<bool>& stop) {
  auto work = std::make_unique<SearchWork>();
  work->pos = root;
  work->pos.fullmove = 1;
  SearchState st = probe_state(limits, stop);
  Value s = negamax(work->pos, *work, st, (SearchPly)ply, depth, alpha, beta);
  ProbeResult pr;
  pr.score = s;
  pr.nodes = st.nodes;
  pr.stopped = st.stopped;
  pr.failed = st.failed;
  return pr;
}

}  // namespace search_test

}  // namespace qwc
