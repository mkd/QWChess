/* QwenChess core: 64-bit bitboard primitives. Header-only.
 *
 * Bit 0 == square a1 (bit index == Square). All operations are total: the zero
 * bitboard and every edge condition are defined. The shift helpers guard the
 * shift amount (never a shift by 64 / negative / out-of-range) and the count/
 * scan helpers return a sentinel on zero, so no call is undefined.
 *
 * The count/scan helpers use the C++20 <bit> standard (std::popcount /
 * std::countr_zero / std::countl_zero) on the normal path, with a
 * straightforward portable fallback. Define QWC_FORCE_PORTABLE_BITS=1 to force
 * the fallback (used by tests to exercise the non-<bit> path on any host).
 * The <bit> scans are themselves total (they return the bit width on zero);
 * QWChess maps zero to its own -1 sentinel -- see lsb_index/msb_index.
 */
#ifndef QWC_CORE_BITBOARD_HPP
#define QWC_CORE_BITBOARD_HPP

#include <bit>
#include <cstdint>
#include "core/types.hpp"

#ifndef QWC_FORCE_PORTABLE_BITS
#define QWC_FORCE_PORTABLE_BITS 0
#endif

/* Full-board occupancy: all 64 squares set. */
constexpr u64 BB_FULL = (u64)0xFFFFFFFFFFFFFFFFULL;

/* Single-square mask. Defined for s in [0,63]; returns 0 for NO_SQUARE / any
 * out-of-range value (no shift by 64). */
constexpr u64 square_bb(Square s) { return s < SQ_NB ? (u64)1 << s : (u64)0; }

/* File mask (a file = 8 stacked squares, no rank wrap) and rank mask
 * (a rank = 8 side-by-side squares, no file wrap). Out-of-range index -> 0.
 * file_bb/rank_bb are the edge masks that prevent file/rank wrapping. */
constexpr u64 file_bb(int f) {
  return (f >= 0 && f < FILE_NB) ? (u64)0x0101010101010101ULL << f : (u64)0;
}
constexpr u64 rank_bb(int r) {
  return (r >= 0 && r < RANK_NB) ? (u64)0xFFULL << (r * 8) : (u64)0;
}

/* Remove the least-significant set bit. pop_lsb(0) == 0 (x-1 underflow on 0
 * is well-defined for unsigned and yields all-ones, so 0 & ~0 == 0). */
constexpr u64 pop_lsb(u64 x) { return x & (x - 1); }

/* Population count. popcount(0) == 0. */
inline int popcount(u64 x) {
#if QWC_FORCE_PORTABLE_BITS
  int c = 0;
  while (x) { c += (int)(x & 1); x >>= 1; }
  return c;
#else
  return std::popcount(x);
#endif
}

/* Index (0..63) of the least-significant set bit, or -1 if x == 0.
 * std::countr_zero(0) is well-defined and returns the bit width (64 for a u64);
 * the explicit zero guard supplies QWChess's own -1 "no set bit" sentinel -- a
 * different value from the standard's 64 -- and keeps the <bit> call on nonzero
 * input. (The guard is a domain choice, not a UB workaround.) */
inline int lsb_index(u64 x) {
  if (x == 0)
    return -1;
#if QWC_FORCE_PORTABLE_BITS
  int i = 0;
  while ((x & 1) == 0) { x >>= 1; i++; }
  return i;
#else
  return std::countr_zero(x);
#endif
}

/* Index (0..63) of the most-significant set bit, or -1 if x == 0. */
inline int msb_index(u64 x) {
  if (x == 0)
    return -1;
#if QWC_FORCE_PORTABLE_BITS
  int i = 0;
  while (x >>= 1) i++;
  return i;
#else
  return 63 - std::countl_zero(x);
#endif
}

#endif /* QWC_CORE_BITBOARD_HPP */
