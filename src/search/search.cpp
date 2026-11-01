/* QwenChess: the deterministic single-worker iterative-deepening search (implementation).
 *
 * See search.hpp for the public contract. The hot path (negamax) is a full-width
 * fail-soft negamax alpha-beta over the complete legal generator, with the temporary
 * Material evaluation at the depth-zero horizon. Interruption is tracked in a
 * `stopped` flag that is never a numeric score: when it is set, the recursion unwinds
 * (every applied move is unmade) and the pending value is discarded, never negated or
 * compared as an evaluation. The per-ply best moves are kept so the last fully
 * completed iteration yields a principal variation.
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
  Move bestmove = move_none();   /* the best move found at this ply (for the PV) */
};

/* The mutable search state: the node count + the interrupt flag + the limit sources
 * (the cancellation is a boolean, never a Value). `stop` is the UCI cancellation flag
 * (polled with relaxed ordering; not owned). */
struct SearchState {
  long long              nodes        = 0;
  bool                   stopped      = false;
  long long              node_limit   = 0;   /* 0 = no node cap */
  bool                   has_deadline = false;
  TimePoint              deadline     {};
  const std::atomic<bool>* stop       = nullptr;
};

/* All per-search hot-path storage, preallocated ONCE into a single heap block for the
 * whole search. No per-node heap allocation; no whole-Position snapshot per ply. The
 * `moves` block is sized for every ply (depth-first: each ply's generated moves must
 * persist while its children are searched). */
struct SearchWork {
  Position                                  pos {};
  std::array<StateInfo, MAX_PLY>            states {};
  std::array<SearchData, MAX_PLY>           sd {};
  std::array<Move, MAX_PLY * kPlyMoveCap>   moves {};
};

/* True when the search must stop: the cancellation flag, the node budget, or the
 * steady-clock deadline has been reached. Sets `st.stopped` (never a score). */
static void check_stop(SearchState& st) {
  if (st.node_limit > 0 && st.nodes >= st.node_limit)
    st.stopped = true;
  if (!st.stopped && st.has_deadline && clock_now() >= st.deadline)
    st.stopped = true;
  if (!st.stopped && st.stop->load(std::memory_order_relaxed))
    st.stopped = true;
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

/* The quiescence search (a fail-soft search over the quiet horizon): in check it searches
 * every legal evasion; otherwise it searches only the captures, with the static score as
 * the stand-pat lower bound. Bounded by MAX_PLY (a static stand-pat at the horizon). The
 * draw policy + the no-legal-move outcomes mirror the negamax. */
static Value qsearch(Position& pos, SearchWork& work, SearchState& st,
                     SearchPly ply, Value alpha, Value beta) {
  if (st.stopped)
    return VALUE_NONE;
  if (ply >= MAX_PLY)
    return eval_material(pos);   /* a bounded stand-pat at the horizon */
  if (pos.halfmove >= 100 || pos_repetition_count(&pos) >= 3)
    return VALUE_ZERO;           /* the draw policy (a search policy) */
  Move* moves = work.moves.data() + ply * kPlyMoveCap;
  std::span<Move> span(moves, kPlyMoveCap);
  std::size_t n = 0;
  if (!movegen_legal(pos, span, n)) {
    st.stopped = true;
    return VALUE_NONE;
  }
  if (n == 0) {
    if (pos_is_in_check(&pos, pos.side))
      return mated_in(ply);      /* we are mated: a root-relative mated score */
    return VALUE_ZERO;           /* stalemate */
  }
  bool in_check = pos_is_in_check(&pos, pos.side);
  Value best = in_check ? -VALUE_INFINITE : eval_material(pos);
  if (!in_check) {
    if (best >= beta)
      return best;               /* a fail-high from the stand-pat */
    if (best > alpha)
      alpha = best;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (st.stopped)
      break;
    Move m = moves[i];
    if (!in_check && !is_capture(pos, m))
      continue;                  /* not in check: only the captures */
    pos_make_move(&pos, &work.states[ply], m);
    Value value = -qsearch(pos, work, st, ply + 1, -beta, -alpha);
    pos_unmake_move(&pos, &work.states[ply]);   /* always unmake: balanced */
    if (st.stopped)
      break;
    if (value > best) {
      best = value;
      if (value > alpha)
        alpha = value;
      if (alpha >= beta)
        break;                   /* a fail-high (a cutoff) */
    }
  }
  return best;
}

/* The fail-soft negamax at `ply`, `depth`. `alpha`/`beta` are the side-to-move
 * perspective window. Returns the node's (fail-soft) score, or a placeholder
 * (VALUE_NONE) when `st.stopped` -- the caller discards it on unwind. */
static Value negamax(Position& pos, SearchWork& work, SearchState& st,
                     int ply, int depth, Value alpha, Value beta) {
  if (st.stopped)
    return VALUE_NONE;
  if (++st.nodes > 0)
    check_stop(st);
  if (st.stopped)
    return VALUE_NONE;

  SearchData& sd = work.sd[ply];
  sd.bestmove = move_none();

  Move* moves = work.moves.data() + ply * kPlyMoveCap;
  std::span<Move> span(moves, kPlyMoveCap);
  std::size_t n = 0;
  if (!movegen_legal(pos, span, n)) {
    st.stopped = true;   /* the position is legal; a generation failure aborts the search */
    return VALUE_NONE;
  }

  /* No legal move: checkmate or stalemate, decided even at depth zero. A mate score
   * beats the draw counter (checked first). */
  if (n == 0) {
    if (pos_is_in_check(&pos, pos.side))
      return mated_in((SearchPly)ply);   /* we (side to move) are mated: root-relative */
    return VALUE_ZERO;                    /* stalemate: a draw */
  }

  /* The draw policy (a search policy, not a FIDE adjudication), after the mate/
   * stalemate check: a position with >=3 occurrences in the real history, or a
   * halfmove clock >=100, is scored 0. No twofold shortcut; no null transition. */
  if (pos.halfmove >= 100 || pos_repetition_count(&pos) >= 3)
    return VALUE_ZERO;

  /* The depth-zero horizon: the quiescence (in check: every evasion; otherwise the
   * captures with the static score as the stand-pat). Reaching depth zero is never a
   * mate (the no-legal-move check above decided that first). */
  if (depth <= 0)
    return qsearch(pos, work, st, (SearchPly)ply, alpha, beta);

  /* Search the moves in the current generator order (no new strength heuristics this
   * milestone; ties break by that order, so the search is deterministic). */
  Value best = -VALUE_INFINITE;   /* strictly below the lowest possible real score */
  for (std::size_t i = 0; i < n; ++i) {
    if (st.stopped)
      break;
    Move m = moves[i];
    pos_make_move(&pos, &work.states[ply], m);
    Value value = -negamax(pos, work, st, ply + 1, depth - 1, -beta, -alpha);
    pos_unmake_move(&pos, &work.states[ply]);   /* always unmake: balanced on every path */
    if (st.stopped)
      break;   /* the child hit a limit; unwind -- `value` (a placeholder) is discarded */
    if (value > best) {
      best = value;
      sd.bestmove = m;
      if (value > alpha)
        alpha = value;
      if (alpha >= beta)
        break;   /* fail-soft beta cutoff */
    }
  }
  /* On unwind `best` may be a stale/placeholder value; the caller discards it when
   * st.stopped. Otherwise it is the node's fail-soft score. */
  return best;
}

/* The root's static depth-0 score (used as the initial score if no iteration
 * completes): mate/stalemate if no legal move, the draw policy, else Material. */
static Value root_static_score(const Position& pos) {
  /* (The caller has already established the root has legal moves for the fallback path;
   * this helper is the general depth-0 score.) */
  if (pos.halfmove >= 100 || pos_repetition_count(&pos) >= 3)
    return VALUE_ZERO;
  return eval_material(pos);
}

SearchResult do_search(Position root, const SearchLimits& limits, std::atomic<bool>& stop) {
  SearchResult res;
  res.bestmove = move_none();
  res.score = VALUE_NONE;
  res.completed_depth = 0;
  res.pv_size = 0;
  res.nodes = 0;
  res.interrupted = false;

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

  /* Generate the root's legal moves. */
  Move* root_moves = work->moves.data();   /* ply 0 */
  std::span<Move> span(root_moves, kPlyMoveCap);
  std::size_t n = 0;
  if (!movegen_legal(work->pos, span, n))
    n = 0;

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

  int max_depth = limits.depth > 0 ? limits.depth : MAX_PLY;
  if (max_depth > MAX_PLY)
    max_depth = MAX_PLY;

  for (int depth = 1; depth <= max_depth; ++depth) {
    if (st.stopped)
      break;
    /* A full-width root search (the reserved-score bounds keep a fail-soft mate/TB
     * value inside its range; a real static score is far inside (−1,1)). */
    Value s = negamax(work->pos, *work, st, 0, depth,
                      VALUE_MATED_IN_MAX_PLY - 1, VALUE_MATE_IN_MAX_PLY + 1);
    if (st.stopped)
      break;   /* this iteration was interrupted; keep the last fully completed result */
    /* This iteration completed fully: record the move, score and PV. If the draw policy
     * fired at the root before the move loop (leaving bestmove unset), fall back to a
     * legal move: a root with legal moves must return one even when a draw is available. */
    if (work->sd[0].bestmove == move_none())
      work->sd[0].bestmove = fallback;
    res.bestmove = work->sd[0].bestmove;   /* a legal move (the generator's choice) */
    res.score = s;
    res.completed_depth = depth;
    res.pv_size = 0;
    for (int p = 0; p < depth && p < MAX_PLY; ++p) {
      if (work->sd[p].bestmove == move_none())
        break;
      res.pv[res.pv_size++] = work->sd[p].bestmove;
    }
    /* A mate / tablebase score cannot improve by searching deeper. */
    if (s >= VALUE_MATE_IN_MAX_PLY || s <= VALUE_MATED_IN_MAX_PLY)
      break;
  }

  res.interrupted = st.stopped;
  res.nodes = st.nodes;
  return res;
}

}  // namespace qwc
