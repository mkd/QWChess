/* QwenChess: FEN parsing / emission for the one Position.
 *
 * Standard 6-field FEN: <placement> <side> <castling> <en-passant> <halfmove>
 * <fullmove>. The parser is strict: it rejects malformed or internally
 * inconsistent FENs (wrong field count, bad placement, a wrong number of kings,
 * castling rights not backed by the board, a structurally invalid en-passant, or
 * an out-of-range / non-canonical counter). On success the loaded Position is
 * rebuilt and passes pos_validate; the recorded en-passant square is retained
 * (even when uncapturable) and only the key/repetition identity canonicalizes it
 * (see position.h). On failure the caller's Position is left UNCHANGED (parsing
 * works on a scratch copy that is only committed on full success).
 *
 * Emission preserves the logical state of every position the parser accepts: the
 * en-passant field mirrors the *recorded* target (the square, or "-") and the
 * castling field is the set of board-backed rights. Placement runs and castling
 * order are normalized; canonical FEN text round-trips byte-for-byte. In
 * particular a FEN whose en-passant square has no
 * legal capture is emitted back with the *same square* it was loaded from (the
 * square is retained, not canonicalized away); only the key/repetition identity
 * ignores an un-capturable target.
 */
#ifndef QWC_POSITION_FEN_H
#define QWC_POSITION_FEN_H

#include "position/position.h"

/* A complete standard FEN is well under this; it bounds the emit buffer. */
#define FEN_BUF_SIZE 256

/* Parse `fen` into *pos. Returns 1 on success (rebuilt + validated, recorded ep
 * retained, key canonicalized). Counters must fit their u16 storage. Returns 0
 * and sets *why (may be NULL) to a short reason on
 * failure, leaving *pos untouched. */
int fen_load(Position *pos, const char *fen, const char **why);

/* Write *pos to `out` as a standard 6-field FEN (single spaces, NUL-terminated),
 * using at most `outsz` bytes. Returns 1 on success, 0 if `out` is NULL or the
 * Position fails pos_validate, or the buffer is too small to hold the FEN plus
 * its terminator (and leaves *out
 * unchanged in that case). A complete standard FEN is far below FEN_BUF_SIZE. */
int fen_emit(const Position *pos, char *out, int outsz);

#endif /* QWC_POSITION_FEN_H */
