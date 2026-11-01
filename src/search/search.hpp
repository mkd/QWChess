/* QwenChess: the deterministic single-worker iterative-deepening search (M2).
 *
 * A correct negamax alpha-beta over full-width moves with the temporary Material
 * evaluation at the horizon (see search/material.hpp). It uses the complete legal
 * generator (movegen_legal), the reversible make/unmake, and the real position
 * history for the draw policy. No TT, no quiescence, no aspiration, no PVS and no new
 * strength heuristics this milestone (those land in T007+). The current move-generator
 * order is the ordering; ties are broken deterministically by that order.
 *
 * The search is a pure, synchronous function (testable without stdin/stdout or a
 * background thread): it copies the caller's root Position, runs to completion or
 * until a limit / the cancellation flag, and returns a SearchResult. The UCI layer
 * (engine/uci.cpp) is what runs it on a worker thread and publishes the result.
 *
 * Ownership/lifetime:
 *   - `root` is passed by value and copied; the caller's Position (and its history
 *     records) are read-only here and must outlive the call (the search's undo records
 *     link onto the root's history chain, which is borrowed, not owned).
 *   - All hot-path storage (the mutable Position, the StateInfo undo array, the
 *     per-ply search data, the per-ply move buffers) is preallocated ONCE into a single
 *     heap block for the whole search (std::unique_ptr); there is no per-node heap
 *     allocation and no whole-Position snapshot per ply.
 *
 * Score convention: Value is the side-to-move perspective. Mate / tablebase reserved
 * ranges are as in core/types.hpp. Mate and stalemate are decided from the no-legal-
 * move set (a root-relative mate distance, or 0). The draw policy (a search policy,
 * not a FIDE adjudication) scores a position 0 when it has >=3 occurrences in the real
 * history OR the halfmove clock is >=100, after the mate/stalemate check. No twofold
 * repetition shortcut; no null move is played or counted.
 */
#ifndef QWC_SEARCH_SEARCH_HPP
#define QWC_SEARCH_SEARCH_HPP

#include <array>
#include <atomic>
#include <chrono>

#include "core/types.hpp"
#include "platform/clock.hpp"
#include "position/position.hpp"

namespace qwc {

/* A conservative bound on the legal moves in one orthodox position (16 pieces x 27
 * destinations + 2 en-passant + 2 castles = 436; the true maximum is well below).
 * Sizes the per-ply move buffers so the generator never overflows here; an overflow
 * would still be a hard failure. */
inline constexpr int kSearchMoveCapacity = 16 * 27 + 2 + 2;   /* 436 */

/* The search limits for one search. Every field is optional (0 / false = no limit).
 * `depth` caps the iterative-deepening depth (0 = no cap, i.e. up to MAX_PLY or the
 * first terminal / no-useful-deeper point). `max_nodes` caps the node count (0 = no
 * cap). `deadline` (steady-clock) is the soft/hard time bound; `has_deadline` gates it.
 * `infinite` marks a `go infinite` / bare `go` (active until stop/quit/replacement). */
struct SearchLimits {
  int         depth       = 0;
  long long   max_nodes   = 0;
  bool        has_deadline = false;
  TimePoint   deadline    {};
  bool        infinite    = false;
};

/* The result of a search. `bestmove` is 0000 (move_none) when the root has no legal
 * move, otherwise a legal move (the last fully completed iteration's best move, or the
 * legal fallback if no iteration completed). `score` is the side-to-move perspective
 * score of the last fully completed iteration (or the root's static depth-0 score if
 * none completed). `interrupted` is set (separately from `score`) when a stop / limit
 * arrived; a cancellation is never represented as, compared as, or used to update a
 * completed score or PV. `pv` holds the PV moves (internal encoding); `pv_size` its
 * length. */
struct SearchResult {
  Move       bestmove         = move_none();
  Value      score            = VALUE_NONE;
  int        completed_depth  = 0;
  int        pv_size          = 0;
  std::array<Move, MAX_PLY>   pv {};
  long long  nodes            = 0;
  bool       interrupted      = false;
};

/* Run the deterministic single-worker iterative-deepening search. `root` is copied
 * (the caller's Position is untouched); `stop` is the UCI cancellation flag (read,
 * not owned, polled with relaxed ordering). The search normalizes ONLY its private
 * fullmove counter to 1 (the u16 game make() overflow guard stays intact for the game
 * history) and keeps the real halfmove + repetition history. It returns the last fully
 * completed iteration, or the legal fallback + the root's static score if none
 * completed. A root with no legal move returns 0000. */
SearchResult do_search(Position root, const SearchLimits& limits, std::atomic<bool>& stop);

}  // namespace qwc

#endif /* QWC_SEARCH_SEARCH_HPP */
