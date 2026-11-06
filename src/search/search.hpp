/* QwenChess: the deterministic single-worker iterative-deepening search (M2).
 *
 * A correct fail-soft negamax alpha-beta over the complete legal generator, with
 * the temporary Material evaluation at the depth-zero horizon (see
 * search/material.hpp) and a quiescence search over the quiet horizon (in check:
 * every legal evasion; otherwise the captures, with the static score as the
 * stand-pat). It uses the reversible make/unmake and the real position history
 * for the draw policy. No TT, no aspiration, no PVS and no new strength
 * heuristics this milestone (those land in later tasks). The current
 * move-generator order is the ordering; ties are broken deterministically by it.
 *
 * The search is a pure, synchronous function (testable without stdin/stdout or a
 * background thread): it copies the caller's root Position, runs to completion or
 * until a limit / the cancellation flag, and returns a SearchResult. The UCI
 * layer (engine/) is what runs it on a worker thread and publishes the result.
 *
 * Ownership/lifetime:
 *   - `root` is passed by value and copied; the caller's Position (and its history
 *     records) are read-only here and must outlive the call (the search's undo
 *     records link onto the root's history chain, which is borrowed, not owned).
 *   - All hot-path storage (the mutable Position, the StateInfo undo array, the
 *     per-ply search data, the per-ply move buffers, the per-ply PV) is
 *     preallocated ONCE into a single heap block for the whole search
 *     (std::unique_ptr); there is no per-node heap allocation and no whole-
 *     Position snapshot per ply.
 *
 * Storage bound (MAX_PLY boundary contract): the documented bound of the search
 * is a node at root-relative ply MAX_PLY (a depth-0 horizon). Every per-ply
 * array is therefore sized MAX_PLY_STACK (= MAX_PLY + 1, root + boundary) and is
 * indexed 0..MAX_PLY only; a node at the boundary never makes a child, so the
 * undo stack and every per-ply buffer stay in bounds.
 *
 * Node-entry contract (shared by negamax and quiescence): every searched node
 * is counted exactly once and polls the node budget / steady-clock deadline /
 * the atomic cancellation flag at entry; a depth-0 node is delegated whole to
 * the quiescence, so the handoff does not double-count. Interruption is tracked
 * in a boolean that is never a numeric score: when set, the recursion unwinds
 * (every applied move is unmade) and the pending placeholder value is
 * discarded, never negated or compared as an evaluation.
 *
 * Terminal/draw contract: a no-legal-move terminal (checkmate / stalemate)
 * precedes the draw policy, which precedes any static evaluation -- including at
 * the depth/ply horizon. The draw policy (a search policy, not a FIDE
 * adjudication) scores a position 0 when it has >=3 occurrences in the real
 * history OR the halfmove clock is >=100. No twofold repetition shortcut; no
 * null move is played or counted.
 *
 * Score convention: Value is the side-to-move perspective in centipawns. The
 * reserved ranges are the pinned Stockfish-19 scheme in core/types.hpp (forced
 * wins score in [±30000, ±30487]; mate_in(1) = 30487; static eval is bounded
 * below that; sentinels above). The root is searched with a true full window
 * ([-VALUE_INFINITE, +VALUE_INFINITE] = ±32000) that contains every real score,
 * including every mate score: a classification threshold such as
 * VALUE_MATE_IN_MAX_PLY (30000) is NOT a search infinity and must never bound
 * the root. Note VALUE_NONE = 0 = VALUE_DRAW: a zero score is ambiguous
 * (draw vs placeholder) and is disambiguated by the state flags, never by
 * value comparison.
 */
#ifndef QWC_SEARCH_SEARCH_HPP
#define QWC_SEARCH_SEARCH_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>

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
 * move, otherwise a legal move (the last fully completed iteration's best move, or
 * the legal fallback if no iteration completed). `score` is a REAL value in the
 * side-to-move perspective: the last fully completed iteration's score, or the
 * root's static depth-0 score if none completed (0 may be a genuine draw). Because
 * VALUE_NONE = 0 = VALUE_DRAW, `score` alone can NEVER detect a failure; the
 * `failed` / `interrupted` flags are the authoritative state. `interrupted` is set
 * (separately from `score`) when a stop / limit arrived; a cancellation is never
 * represented as, compared as, or used to update a completed score or PV. `failed`
 * is set on an INTERNAL error (a move generation or make failure on a legal
 * position): it is distinct from a cancellation, is never relabelled a terminal
 * (a failed root generator is not a stalemate / checkmate), and keeps the legal
 * fallback when one was known. `pv` holds the root's PV line (each node snapshots
 * [its best move] + the selected child's continuation; the root row is a real,
 * replayable line), `pv_size` its length. */
struct SearchResult {
  Move       bestmove         = move_none();
  Value      score            = VALUE_NONE;
  int        completed_depth  = 0;
  int        pv_size          = 0;
  std::array<Move, MAX_PLY>   pv {};
  long long  nodes            = 0;
  bool       interrupted      = false;
  bool       failed           = false;
};

/* An optional completed-iteration observer (progress reporting for the UCI layer).
 * `on_iteration` is invoked (on the search thread) ONLY after a fully completed
 * result / PV has been saved into `res`. It is synchronous, worker-thread-only, and
 * must not retain references to `res` or its PV beyond the call. At most one call per
 * completed iteration (never per node); a null `on_iteration` is a no-op, so the
 * search itself stays protocol-free. */
struct SearchObserver {
  void (*on_iteration)(const SearchResult&, void*) = nullptr;
  void* user = nullptr;
};

/* Run the deterministic single-worker iterative-deepening search. `root` is copied
 * (the caller's Position is untouched); `stop` is the UCI cancellation flag (read,
 * not owned, polled with relaxed ordering). The search normalizes ONLY its private
 * fullmove counter to 1 (the u16 game make() overflow guard stays intact for the game
 * history) and keeps the real halfmove + repetition history. It returns the last fully
 * completed iteration, or the legal fallback + the root's static score if none
 * completed. A root with no legal move returns 0000. `observer` (default no-op) is
 * called after each completed iteration is saved. */
SearchResult do_search(Position root, const SearchLimits& limits, std::atomic<bool>& stop,
                       const SearchObserver& observer = SearchObserver{});

/* A white-box test seam (NOT an engine feature): it gives tests direct access to the
 * internal node functions at an arbitrary root-relative ply / depth. The shipped
 * engine never calls these; only tests use them. A probe copies `root` into a fresh
 * per-search arena (a normalized private fullmove, a fresh node counter), so the
 * caller's Position is untouched; `limits` + `stop` mirror the do_search meanings. */
namespace search_test {

struct ProbeResult {
  Value     score   = VALUE_NONE;   /* the node's (fail-soft) score, or a placeholder */
  long long nodes   = 0;            /* the nodes searched (the entry is counted once) */
  bool      stopped = false;        /* a limit / cancellation was reached */
  bool      failed  = false;        /* an internal error (generation / make) */
};

/* Run the internal quiescence on a copy of `root` at root-relative `ply`. */
ProbeResult qsearch_probe(Position root, SearchPly ply, Value alpha, Value beta,
                          const SearchLimits& limits, std::atomic<bool>& stop);

/* Run the internal negamax on a copy of `root` at (ply, depth). */
ProbeResult negamax_probe(Position root, int ply, int depth, Value alpha, Value beta,
                          const SearchLimits& limits, std::atomic<bool>& stop);

}  // namespace search_test

}  // namespace qwc

#endif /* QWC_SEARCH_SEARCH_HPP */
