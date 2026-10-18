/* QwenChess: FEN parsing / emission for the one Position.
 *
 * Standard 6-field FEN: <placement> <side> <castling> <en-passant> <halfmove>
 * <fullmove>. The parser is strict: it rejects malformed or internally
 * inconsistent FENs (wrong field count, bad placement, a wrong number of kings,
 * castling rights not backed by the board, a structurally invalid en-passant, or
 * an out-of-range / non-canonical counter). On success the loaded Position is
 * rebuilt and passes pos_validate; the recorded en-passant square is retained
 * (even when uncapturable) and only the key/repetition identity canonicalizes it
 * (see position.hpp). On failure the caller's Position is left UNCHANGED (parsing
 * works on a scratch copy that is only committed on full success).
 *
 * Emission preserves the logical state of every position the parser accepts: the
 * en-passant field mirrors the *recorded* target (the square, or "-") and the
 * castling field is the set of board-backed rights. Placement runs and castling
 * order are normalized; canonical FEN text round-trips byte-for-byte. In
 * particular a FEN whose en-passant square has no legal capture is emitted back
 * with the *same square* it was loaded from (the square is retained, not
 * canonicalized away); only the key/repetition identity ignores an un-capturable
 * target.
 *
 * Interfaces: the *primary* overloads are the bounded C++20 forms (std::string_view
 * input / std::span output, borrowed views). The pointer-based overloads are
 * documented compatibility adapters that validate their legacy null/signed cases
 * and then delegate to the same primary -- there is exactly one parser and one
 * emitter. The borrowed views are never stored in Position/StateInfo/a global/a
 * later callback.
 */
#ifndef QWC_POSITION_FEN_HPP
#define QWC_POSITION_FEN_HPP

#include "position/position.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace qwc {

/* A complete standard FEN is well under this; it bounds the emit buffer. */
constexpr int FEN_BUF_SIZE = 256;

/* ---- primary (bounded) interfaces ----------------------------------------- */

/* Parse `text` -- the [data, data+size) range, a NUL terminator NOT required --
 * into `pos`. `text` borrows caller-owned storage for the duration of the call and
 * is never stored in the Position. Returns true on success (rebuilt + validated; a
 * successful setup starts a FRESH history) and false on failure, leaving `pos` --
 * and its existing history head -- byte-for-byte unchanged.
 *
 * The parser honors the view's exact size: it never calls strlen, never scans to an
 * assumed terminator, and rejects empty input and any NUL byte inside the range
 * (a terminating NUL accidentally included in the view is input data and is
 * rejected). The strict grammar is unchanged: exactly six nonempty fields separated
 * by five single ASCII spaces (no leading/trailing/doubled spaces, signs, leading
 * counter zeroes, or trailing junk); placement is eight ranks of eight squares;
 * castling is "-" or any order of KQkq with no repeats; the counters are canonical
 * base-10 in their u16 ranges, validated before narrowing.
 *
 * `error` is optional (default null): when non-null it is cleared on entry and, on
 * failure, set to a nonempty diagnostic backed by a static string literal (so it
 * outlives the call); on success it is left empty. It never points into a
 * temporary or into the caller's input. */
[[nodiscard]] bool fen_load(Position& pos, std::string_view text,
                            std::string_view* error = nullptr);

/* Write `pos` into `output` -- a borrowed reference to caller-owned storage, valid
 * for the duration of the call -- as a standard 6-field FEN (single spaces,
 * NUL-terminated). Returns true on success and false -- leaving `output` untouched --
 * if `pos` fails pos_validate or the buffer cannot hold the FEN plus its
 * terminator. The entire required length (including the terminator) is checked
 * against output.size() before any byte is written, so an empty or one-byte-short
 * buffer fails; the unused suffix of `output` is left unchanged on success. */
[[nodiscard]] bool fen_emit(const Position& pos, std::span<char> output);

/* ---- compatibility adapters (delegate to the primaries) ------------------- */

/* C-string adapter for the primary fen_load. Requires `fen` to be NUL-terminated
 * when non-null; its strlen excludes the terminator, so the view handed to the
 * primary never contains the NUL. Returns 1 on success, 0 on failure; on failure
 * a non-null `why` receives a static-backed reason (and nullptr on success). A null
 * `pos` fails (silently, as before); a null `fen` fails with a "nullptr FEN"
 * diagnostic, preserving the legacy behavior. */
int fen_load(Position *pos, const char *fen, const char **why);

/* Buffer adapter for the primary fen_emit. `outsz` is a signed byte capacity; a
 * null `out`/`pos` or a nonpositive `outsz` fails before any conversion. Returns
 * 1 on success, 0 otherwise (and leaves `out` untouched on failure). */
int fen_emit(const Position *pos, char *out, int outsz);

}  // namespace qwc

#endif /* QWC_POSITION_FEN_HPP */
