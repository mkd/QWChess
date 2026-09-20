/* QwenChess core: 64-bit bitboard primitives. Header-only (static inline).
 *
 * Bit 0 == square a1 (bit index == Square). All operations are total: the
 * zero bitboard and edge conditions are defined, and no call performs an
 * undefined shift (no shift by 64 / negative shift / zero-input bit-scan).
 *
 * The count/scan helpers use compiler builtins where available, with a
 * straightforward portable fallback. Define QWC_FORCE_PORTABLE_BITS=1 to force
 * the fallback (used by tests to exercise the non-builtin path on any host).
 */
#ifndef QWC_CORE_BITBOARD_H
#define QWC_CORE_BITBOARD_H

#include <stdint.h>
#include "core/types.h"

#ifndef QWC_FORCE_PORTABLE_BITS
#define QWC_FORCE_PORTABLE_BITS 0
#endif

/* Full-board occupancy: all 64 squares set. */
#define BB_FULL ((u64)0xFFFFFFFFFFFFFFFFULL)

/* Single-square mask. Defined for s in [0,63]; returns 0 for NO_SQUARE / any
 * out-of-range value (no shift by 64). */
static inline u64 square_bb(Square s) { return s < SQ_NB ? (u64)1 << s : (u64)0; }

/* File mask (a file = 8 stacked squares, no rank wrap) and rank mask
 * (a rank = 8 side-by-side squares, no file wrap). Out-of-range index -> 0.
 * file_bb/rank_bb are the edge masks that prevent file/rank wrapping. */
static inline u64 file_bb(int f) {
  return (f >= 0 && f < FILE_NB) ? (0x0101010101010101ULL << f) : (u64)0;
}
static inline u64 rank_bb(int r) {
  return (r >= 0 && r < RANK_NB) ? (0xFFULL << (r * 8)) : (u64)0;
}

/* Remove the least-significant set bit. pop_lsb(0) == 0 (x-1 underflow on 0
 * is well-defined for unsigned and yields all-ones, so 0 & ~0 == 0). */
static inline u64 pop_lsb(u64 x) { return x & (x - 1); }

/* Population count. popcount(0) == 0. */
static inline int popcount(u64 x) {
#if QWC_FORCE_PORTABLE_BITS
  int c = 0;
  while (x) { c += (int)(x & 1); x >>= 1; }
  return c;
#else
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_popcountll(x);
#else
  int c = 0;
  while (x) { c += (int)(x & 1); x >>= 1; }
  return c;
#endif
#endif
}

/* Index (0..63) of the least-significant set bit, or -1 if x == 0.
 * The builtin is only called on nonzero input (its zero-input behavior is UB). */
static inline int lsb_index(u64 x) {
  if (x == 0)
    return -1;
#if QWC_FORCE_PORTABLE_BITS
  int i = 0;
  while ((x & 1) == 0) { x >>= 1; i++; }
  return i;
#else
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_ctzll(x);
#else
  int i = 0;
  while ((x & 1) == 0) { x >>= 1; i++; }
  return i;
#endif
#endif
}

/* Index (0..63) of the most-significant set bit, or -1 if x == 0. */
static inline int msb_index(u64 x) {
  if (x == 0)
    return -1;
#if QWC_FORCE_PORTABLE_BITS
  int i = 0;
  while (x >>= 1) i++;
  return i;
#else
#if defined(__GNUC__) || defined(__clang__)
  return 63 - __builtin_clzll(x);
#else
  int i = 0;
  while (x >>= 1) i++;
  return i;
#endif
#endif
}

#endif /* QWC_CORE_BITBOARD_H */
