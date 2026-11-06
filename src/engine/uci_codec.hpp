/* QwenChess: the production UCI coordinate-move codec.
 *
 * The single move<->text codec for the UCI protocol, built above position + movegen
 * (position/core never depend on it). It is reused for `position ... moves` input,
 * `bestmove` output and `info ... pv` output. It is NOT the diagnostic +c/+e/=Q
 * notation (that is test-only) and does not change the internal Move representation:
 *
 *   ordinary      e2e4            (4 chars: from-file,from-rank,to-file,to-rank)
 *   promotion     a7a8q           (5 chars: + lowercase q/r/b/n)
 *   castle (wire) e1g1 e1c1 e8g8  (the KING's g/c destination, NOT the internal one)
 *                 e8c8
 *   no-move       0000            (the output sentinel for a root with no legal move)
 *
 * The internal castle encoding stays king-origin -> home-ROOK origin (e1h1 / e1a1 /
 * e8h8 / e8a8 with MV_CASTLE); only the text on the wire uses the king's g/c
 * destination. make/unmake are untouched.
 *
 * Parse resolution: an input token is resolved against the COMPLETE legal move set of
 * the position (movegen_legal), so en-passant / promotion / castle flags are derived
 * correctly and a pinned or otherwise illegal move is rejected (it simply is not in
 * the legal set). A candidate matches when its CANONICAL WIRE form equals the token
 * squares: a castle's wire 'to' is the king's g/c destination, every other move its
 * ordinary destination -- the token's squares are NEVER remapped to internal
 * encodings, so an ordinary rook/queen move from a home-king square (e1-g1 with the
 * king elsewhere) is accepted as itself, and the internal castle spelling (e1-h1) is
 * rejected. 0000 is rejected as a playable move (it is only the output no-move
 * sentinel; there is no playable null move).
 *
 * Failure contract (tested): on any parse failure `out` is left UNCHANGED and the
 * call returns false; on success `out` is set and the call returns true. A failed
 * parse therefore never clobbers the caller's Move.
 */
#ifndef QWC_ENGINE_UCI_CODEC_HPP
#define QWC_ENGINE_UCI_CODEC_HPP

#include <cstddef>
#include <span>
#include <string_view>

#include "core/square.hpp"
#include "core/types.hpp"
#include "movegen/movegen.hpp"
#include "position/position.hpp"

namespace qwc {

/* A conservative bound on the number of legal moves in one orthodox position
 * (16 pieces x 27 destinations + 2 en-passant + 2 castles = 436; the true maximum
 * is well below this). Sizing the legal-set buffer to this makes movegen_legal()
 * never overflow here; an overflow is still a hard failure. */
inline constexpr int kUciMoveCapacity = 16 * 27 + 2 + 2;   /* 436 */

/* Maximum UCI move-text length: 5 characters ("a7a8q") plus the NUL terminator. */
inline constexpr int kUciMoveText = 6;

/* A character -> promotion PieceType payload (2..5). Accepts lowercase (the
 * documented UCI form) and, for robustness, uppercase. Any other character (a 'p',
 * a digit, 'k', or the like) yields 0 (invalid). */
[[nodiscard]] u32 uci_promo_from_char(char c);

/* A promotion PieceType payload (2..5) -> its lowercase wire character. Any other
 * payload yields 0 (a non-promotion never carries a payload). */
[[nodiscard]] char uci_char_from_promo(u32 promotion_type);

/* Parse a UCI move token against the complete legal move set of `pos`.
 *
 * `token` is the [data, data+size) range (a NUL terminator is not required). A valid
 * token is exactly 4 characters ("e2e4") or 5 characters with a lowercase promotion
 * piece ("a7a8q"). The token is resolved against movegen_legal(pos): a matching legal
 * move (same from/to, and the same promotion payload -- zero for non-promotions) is
 * written to `out` and true is returned. En-passant, castle and promotion flags come
 * from the matching legal move, so they are derived correctly; a pinned or illegal
 * token matches nothing and fails. "0000" and any other malformed token (bad squares,
 * extra payload, a 5th non-promotion character, a 4th/5th square that is a pawn's own
 * forward push spelled without a promotion, etc.) fail.
 *
 * On failure `out` is left unchanged and false is returned (see the header contract).
 * `pos` must be an initialized, consistent Position; it is left untouched. */
[[nodiscard]] bool uci_move_parse(const Position& pos, std::string_view token, Move& out);

/* Emit `m` as UCI text into `buf` (NUL-terminated) and return its length in
 * characters (not counting the NUL). The buffer must hold at least kUciMoveText
 * bytes (5 chars + NUL); if `cap < kUciMoveText` nothing is written and 0 is
 * returned. Ordinary moves and en-passant use the 4-char form; a promotion appends
 * its lowercase piece (5 chars); a castle emits the king's g/c destination (the
 * internal rook-origin is never leaked); move_none() (the no-move sentinel) is
 * EXPLICITLY emitted as the 4-char "0000" (it is a root with no legal move --
 * checkmate / stalemate -- or an internal failure with no known move; note that
 * move_from(move_none()) is square 0 = a1, so it must never fall through to the
 * ordinary path, which would emit a bogus "a1a1"). The result is exactly the
 * documented UCI form and round-trips through uci_move_parse on the destination
 * position (a "0000" output is a sentinel, not a parseable move). */
[[nodiscard]] int uci_move_emit(Move m, char* buf, std::size_t cap);

}  // namespace qwc

#endif /* QWC_ENGINE_UCI_CODEC_HPP */
