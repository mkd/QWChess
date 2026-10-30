/* QwenChess: perft / divide -- implementation. See perft.hpp for the contract.
 *
 * One traversal (perft_rec) does all the counting. The root perft() validates the
 * depth, copies the Position into a single diagnostic scratch (counters normalized,
 * history detached), and drives the recursion against a bounded, stable undo stack.
 *
 * The recursion is deliberately simple: at every traversed edge (including the
 * final ply) it makes the move, counts the child, and unmakes it before continuing
 * or propagating a failure. There is no full Position rebuild or pos_validate in the
 * counting loop; those are audited separately in the tests and in make/unmake.
 */
#include "diagnostics/perft.hpp"

#include <array>
#include <limits>

#include "movegen/movegen.hpp"

namespace qwc {

namespace {

/* One node's bounded move storage (kPerftMoveCapacity Moves, ~1.7 KiB). A
 * std::span over this array is what movegen_legal() writes into. */
struct PerftMoves {
  std::array<Move, kPerftMoveCapacity> m{};
};

/* The recursive counter. `scratch` is the single mutable diagnostic board (owned
 * by the root perft() call); `depth` is the remaining plies; `ply` is how many
 * moves have been made from the scratch root (it indexes `undo`). `undo` is the
 * caller's stable, bounded StateInfo stack (MAX_PLY records; the root of the
 * detached scratch history is nullptr). `nodes` accumulates the running count.
 *
 * Returns true on success. Returns false on a move-generation failure, a make
 * failure, or a checked u64 overflow; the caller unwinds any move it made and
 * propagates the failure. A successful make is ALWAYS unmaked before the function
 * returns (including on the failure path). */
bool perft_rec(Position& scratch, int depth, int ply,
               std::array<StateInfo, MAX_PLY>& undo, u64& nodes) {
  if (depth == 0) {
    // Count this node (one, whether or not it is terminal). Checked overflow: a
    // saturated counter would make the next increment wrap, so fail instead.
    if (nodes == std::numeric_limits<u64>::max()) return false;
    ++nodes;
    return true;
  }

  // Defensive bound. Because depth is validated to <= MAX_PLY and depth+ply is
  // invariant (= the original depth), depth > 0 implies ply < MAX_PLY here, so this
  // is unreachable in practice; it keeps the undo index in-bounds if ever not.
  if (ply >= MAX_PLY) return false;

  PerftMoves moves;
  std::size_t count = 0;
  if (!movegen_legal(scratch, moves.m, count)) return false;

  // A positive-depth node with no legal moves contributes 0.
  if (count == 0) return true;

  for (std::size_t i = 0; i < count; ++i) {
    StateInfo& rec = undo[ply];
    if (!pos_make_move(&scratch, &rec, moves.m[i])) return false;
    const bool ok = perft_rec(scratch, depth - 1, ply + 1, undo, nodes);
    pos_unmake_move(&scratch, &rec);   // always unwind a made move, success or not
    if (!ok) return false;
  }
  return true;
}

}  // namespace

bool perft(const Position& position, int depth, u64& nodes) {
  nodes = 0;

  // Validate the depth before any traversal: reject negatives and anything above
  // the search bound. This is the only depth gate; the recursion does not re-check.
  if (depth < 0 || depth > MAX_PLY) return false;

  // depth 0 is the position itself: exactly one node (terminal or not).
  if (depth == 0) {
    nodes = 1;
    return true;
  }

  // Copy the Position once. Normalize only the diagnostic scratch's bookkeeping
  // counters and detach its history (the count must not depend on the caller's
  // rule-50/fullmove counters or on its live repetition history). The board,
  // castling rights, recorded en-passant square and key are left intact; the key is
  // derived from the board/rights/ep/side (not the counters), so normalizing the
  // counters leaves it correct. The caller's Position and its history records are
  // untouched.
  Position scratch = position;
  scratch.halfmove = 0;
  scratch.fullmove = 1;
  scratch.history = nullptr;

  // A stable, bounded undo stack (one StateInfo per outstanding move). The root of
  // the detached scratch history is nullptr; makes link these local records into it
  // and unmakes restore it, so the caller's records are never touched.
  std::array<StateInfo, MAX_PLY> undo{};

  const bool ok = perft_rec(scratch, depth, 0, undo, nodes);
  if (!ok) {
    nodes = 0;   // a failed traversal is never reported as a valid zero
    return false;
  }
  return true;
}

}  // namespace qwc
