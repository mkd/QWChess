/* QwenChess: legal move generation.
 *
 * Three generators live here, one dependency (movegen -> position/core only;
 * position/core never depend on movegen). All emit complete, deterministic,
 * duplicate-free encoded Move lists; none emits the null move.
 *
 *  - movegen_pseudo_noncastling (cp1): every non-castling pseudo-legal candidate.
 *    "Pseudo-legal" means correct piece geometry, occupancy and special-move
 *    metadata; own-king safety and resolving check are deliberately NOT filtered
 *    (a king may move to an attacked square and a pinned captor may take). This
 *    generator is READ-ONLY over the Position: it performs no make/unmake, no
 *    full validation or rebuild, no king-safety query, and no heap allocation.
 *
 *  - movegen_legal_noncastling (cp2): the pseudo-legal candidates with own-king
 *    safety applied. It keeps only the moves that leave the ORIGINAL mover's own
 *    king unattacked. To do that it probes a scratch copy of the board (one per
 *    call): each candidate is applied, the mover's king is checked on the
 *    resulting board, and the move is reversed. That scratch + a local undo record
 *    are bounded stack-locals, not a second authoritative board and not per-move
 *    heap; the caller's Position is left untouched.
 *
 *  - movegen_legal (cp3): the COMPLETE orthodox legal set -- the non-castling legal
 *    moves plus the legal castles. A castle (king origin -> rook origin, MV_CASTLE)
 *    is legal iff the side has the right, the home king/rook are in place with an
 *    empty path, and the king's ORIGIN, TRANSIT (one rank toward the rook) and
 *    DESTINATION squares are all unattacked (giving check is legal). The origin is
 *    a read-only check; the transit and destination are probed on one normalized
 *    scratch copy via existing make/unmake. The two non-castling APIs above keep
 *    their contracts and continue to EXCLUDE every castle.
 *
 * All three are bounded (a std::span output buffer + a size_t& count); the
 * pseudo-legal generator has an additional documented legacy pointer adapter.
 * SEE, evasions and staged search ordering are later milestones (M2/M5) and are
 * not part of any generator. No new board representation, NNUE fields, runtime
 * dispatch framework, sorting, SIMD or magic attacks are introduced.
 */
#ifndef QWC_MOVEGEN_MOVEGEN_HPP
#define QWC_MOVEGEN_MOVEGEN_HPP

#include <cstddef>
#include <span>

#include "position/position.hpp"

namespace qwc {

/* Generate every non-castling pseudo-legal candidate of `pos` into `output`.
 *
 * This is the primary (bounded) form. `output` borrows caller-owned storage of
 * `output.size()` Moves for the duration of the call; `count` is written.
 *
 * Contract:
 *  - `pos` is an initialized, consistent Position (coherent derived caches) as
 *    produced by the setup/make paths; `output` is a nonoverlapping buffer.
 *    `count` is always written (it is a required reference, not an optional
 *    pointer).
 *  - A default-constructed empty span is a valid empty range: the generation
 *    succeeds with `count == 0` only when the position generates zero candidates;
 *    otherwise the first failed append makes the call fail with `count == 0`
 *    (nothing is written to a zero-size span).
 *  - Capacity is checked before every append. No Move is written outside the
 *    supplied capacity and a successful result is never truncated. On overflow an
 *    in-bounds partial prefix may remain in `output` but is unusable; no second
 *    generation pass is made to preserve it. `count` is zero on every failure and
 *    the complete move count on success.
 *  - The Position (board, key, counters and the complete history chain) is left
 *    untouched.
 *  - Enumeration is deterministic and duplicate-free. The order is an internal
 *    convenience and is NOT a public search-ordering contract.
 *
 * Returns true on success, false on overflow. */
[[nodiscard]] bool movegen_pseudo_noncastling(const Position& pos,
                                              std::span<Move> output,
                                              std::size_t& count);

/* Compatibility adapter: the legacy pointer form. `pos` and `out` are
 * caller-supplied (a null `out` fails even with zero capacity; a null `pos`
 * fails); `capacity` is the buffer's size in Moves; `count` is OPTIONAL (it may
 * be nullptr). Returns 1 on success and, when `count` is non-nullptr, writes the
 * complete move count (zero for an empty result); returns 0 on failure and, when
 * `count` is non-nullptr, sets it to zero. The contract is otherwise identical to
 * the primary above. It validates its null pointers before constructing the span
 * and delegates to the same primary generator. */
int movegen_pseudo_noncastling(const Position *pos,
                               Move *out, std::size_t capacity,
                               std::size_t *count);

/* Generate every non-castling LEGAL move of `pos` into `output` (T005 cp2; the
 * primary, bounded form). A move is legal iff it is pseudo-legal AND leaving the
 * ORIGINAL mover's own king unattacked on the resulting board (giving check is
 * legal). This is the cp2 king-safety filter over the pseudo-legal candidates.
 *
 * `output` borrows caller-owned, nonoverlapping storage of `output.size()` Moves for
 * the duration of the call; `count` is written.
 *
 * Contract:
 *  - `pos` is an initialized, consistent orthodox Position satisfying the existing
 *    setup/validation invariants (including at most sixteen pieces per color). No
 *    full re-validation is performed on every call.
 *  - `output` may be a zero-size span. The Position -- its board, key, derived
 *    caches, counters, history head, and every live caller-owned history record --
 *    is left untouched; the filter works on a scratch copy (a bounded stack-local
 *    per call; no per-move heap allocation).
 *  - Success returns true with the COMPLETE non-castling legal count. The order is
 *    deterministic and duplicate-free; it is an internal convenience and is NOT a
 *    public search-ordering promise.
 *  - An empty span succeeds with `count == 0` only when no non-castling legal move
 *    exists (e.g. checkmate or stalemate); otherwise the first failed append
 *    returns false with `count == 0` (nothing is written to a zero-size span).
 *  - Insufficient output capacity returns false with `count == 0`. Nothing is ever
 *    written outside the span; an in-bounds partial prefix may remain but is
 *    unusable.
 *  - An unexpected internal candidate/application failure also returns false with
 *    `count == 0`; a candidate is never silently omitted because make failed.
 *  - Null moves and castling are excluded. Draw adjudication, SEE, evaluation and
 *    search pruning are not part of this API.
 *
 * Returns true on success, false on overflow or an internal application error. */
[[nodiscard]] bool movegen_legal_noncastling(const Position& pos,
                                              std::span<Move> output,
                                              std::size_t& count);

/* Generate every orthodox LEGAL move of `pos` into `output` (T005 cp3; the
 * primary, bounded form). The result is the complete legal set: every
 * non-castling legal move (movegen_legal_noncastling) plus every legal castle.
 * A move is legal iff it is pseudo-legal and leaves the ORIGINAL mover's own king
 * unattacked on the resulting board (giving a check is legal). A castle (the
 * frozen king-origin -> rook-origin encoding with exactly MV_CASTLE and zero
 * promotion payload) is legal iff the side holds the right, the home king and
 * rook are in place with an empty path (short: f/g; long: b/c/d), and the king's
 * origin, transit (one square toward the rook) and destination are all
 * unattacked. The queenside b/c file need only be empty -- an attacked b-square or
 * rook square does not forbid castling; only the king's three squares are tested.
 *
 * `output` borrows caller-owned, nonoverlapping storage of `output.size()` Moves
 * for the duration of the call; `count` is written.
 *
 * Contract:
 *  - `pos` is an initialized, consistent orthodox Position satisfying the existing
 *    setup/validation invariants (including at most sixteen pieces per color). No
 *    full re-validation is performed on every call. Chess960 is not supported.
 *  - `output` may be a zero-size span. The Position -- its board, key, derived
 *    caches, counters, history head, and every live caller-owned history record --
 *    is left untouched on every outcome; the castling probes work on a scratch
 *    copy (a bounded stack-local per call; no per-move heap allocation) and no
 *    borrowed view is stored.
 *  - Success returns true with the COMPLETE legal count (`count == output.size()`
 *    is the number of moves written). The order is deterministic (non-castling
 *    moves in the cp2 order, then the kingside castle, then the queenside castle,
 *    where each is legal) and duplicate-free; it is an internal convenience and is
 *    NOT a public search-ordering promise.
 *  - An empty span succeeds with `count == 0` only when the complete legal set is
 *    empty (checkmate or stalemate); otherwise the first failed append returns
 *    false with `count == 0` (nothing is written to a zero-size span).
 *  - Insufficient output capacity returns false with `count == 0`. This includes a
 *    buffer that fits every non-castling move but not the full set once castles are
 *    added. Nothing is ever written outside the span; an in-bounds partial prefix
 *    may remain but is unusable.
 *  - An unexpected internal application failure (a make that should succeed after
 *    the structural prechecks) also returns false with `count == 0`; a candidate is
 *    never silently omitted because make failed.
 *  - Legal moves are independent of the game's rule-50 / fullmove counter storage
 *    limits and of repetition/draw adjudication. The real game make() overflow
 *    guards are preserved (only the probes normalize their scratch counters).
 *  - Null moves and draw adjudication, SEE, evaluation and search pruning are not
 *    part of this API.
 *
 * Returns true on success, false on overflow or an internal application error. */
[[nodiscard]] bool movegen_legal(const Position& pos,
                                 std::span<Move> output,
                                 std::size_t& count);

}  // namespace qwc

#endif /* QWC_MOVEGEN_MOVEGEN_HPP */
