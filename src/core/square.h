/* QwenChess core: square / file / rank helpers. Header-only.
 *
 * Square convention (from types.h): a1=0 .. h8=63, s = rank*8 + file
 * (0-indexed; file a=0..h=7, rank 1=0..8=7). NO_SQUARE == 64 is the sentinel.
 *
 * The index functions (file_of / rank_of) are total on the u8 domain but are
 * only *meaningful* for valid squares [0,63]; for a sentinel/invalid square
 * they return out-of-range components (e.g. rank_of(NO_SQUARE)==8). Callers
 * must guard with is_valid_square() before relying on the values.
 */
#ifndef QWC_CORE_SQUARE_H
#define QWC_CORE_SQUARE_H

#include "core/types.h"

static inline int file_of(Square s) { return (int)(s & 7); }   /* a=0 .. h=7 */
static inline int rank_of(Square s) { return (int)(s >> 3); }  /* rank1=0 .. rank8=7 */

/* Caller must ensure file,rank are each in [0,7]. */
static inline Square square_of(int file, int rank) {
  return (Square)((rank << 3) | file);
}

/* A square is valid iff it is a board square; NO_SQUARE (64) and above are not. */
static inline int is_valid_square(Square s) { return s < SQ_NB; }

static inline char file_char(int f) { return (char)('a' + f); }
static inline char rank_char(int r) { return (char)('1' + r); }

/* Write "a1".."h8" into out (3 bytes incl NUL). Returns 1 on success,
 * 0 (and leaves out untouched) for an invalid/sentinel square. */
static inline int square_name(Square s, char out[3]) {
  if (!is_valid_square(s))
    return 0;
  out[0] = (char)('a' + (s & 7));
  out[1] = (char)('1' + (s >> 3));
  out[2] = '\0';
  return 1;
}

#endif /* QWC_CORE_SQUARE_H */
