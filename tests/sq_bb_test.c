/* QwenChess T002 test -- group 1: square/file/rank + bitboard primitives.
 *
 * Validates the most foundational primitives only. Later groups (move, attacks,
 * zobrist) are intentionally NOT exercised here. Checks are always active
 * (never NDEBUG-guarded) so they hold in release and sanitizer builds alike.
 *
 * Covered: square/file/rank conversion + sentinel behavior; single-bit masks at
 * a1/a8/h1/h8; popcount; lsb/msb index; least-bit removal; file/rank edge masks
 * (no wrap). Zero-input and sentinel cases are defined for every operation.
 */
#include "core/types.h"
#include "core/square.h"
#include "core/bitboard.h"
#include "test_util.h"

/* ---- square / file / rank ---- */
static int t_square(void) {
  qwc_begin("square");
  char buf[3];

  /* Every board square round-trips exactly through file_of/rank_of/square_of. */
  for (int s = 0; s < SQ_NB; s++) {
    int f = file_of((Square)s), r = rank_of((Square)s);
    QWC_EQ_I(f, s & 7, "file_of range");
    QWC_EQ_I(r, s >> 3, "rank_of range");
    QWC_EQ_I((int)square_of(f, r), s, "square_of round-trip");
    QWC_TRUE(is_valid_square((Square)s), "board square is valid");
  }

  /* Sentinel / out-of-range squares. */
  QWC_TRUE(!is_valid_square(NO_SQUARE), "NO_SQUARE invalid");
  QWC_TRUE(!is_valid_square(64 + 1), "65 invalid");
  QWC_TRUE(!is_valid_square(255), "255 invalid");
  QWC_EQ_I(is_valid_square(63), 1, "h8 valid");

  /* square_name: the four corners, a mid square, and the sentinel. */
  QWC_TRUE(square_name(0, buf) && buf[0] == 'a' && buf[1] == '1' && buf[2] == 0, "a1 name");
  QWC_TRUE(square_name(63, buf) && buf[0] == 'h' && buf[1] == '8', "h8 name");
  QWC_TRUE(square_name(56, buf) && buf[0] == 'a' && buf[1] == '8', "a8 name");
  QWC_TRUE(square_name(7, buf) && buf[0] == 'h' && buf[1] == '1', "h1 name");
  QWC_TRUE(square_name(36, buf) && buf[0] == 'e' && buf[1] == '5', "e5 name");
  QWC_TRUE(!square_name(NO_SQUARE, buf), "sentinel name -> 0");

  return qwc_end();
}

/* ---- bitboard primitives ---- */
static int t_bitboard(void) {
  qwc_begin("bitboard");

  /* Single-bit masks: corners + sentinel / out-of-range. */
  QWC_EQ_U64(square_bb(0), (u64)1, "square_bb a1 = bit0");
  QWC_EQ_U64(square_bb(7), (u64)1 << 7, "square_bb h1 = bit7");
  QWC_EQ_U64(square_bb(56), (u64)1 << 56, "square_bb a8 = bit56");
  QWC_EQ_U64(square_bb(63), (u64)1 << 63, "square_bb h8 = bit63");
  QWC_EQ_U64(square_bb(NO_SQUARE), 0, "square_bb sentinel -> 0");
  QWC_EQ_U64(square_bb(255), 0, "square_bb 255 -> 0");

  /* popcount: zero, single bit, full board, one file. */
  QWC_EQ_I(popcount(0), 0, "popcount(0)");
  QWC_EQ_I(popcount((u64)1 << 63), 1, "popcount single high");
  QWC_EQ_I(popcount(BB_FULL), 64, "popcount full board");
  QWC_EQ_I(popcount(file_bb(0)), 8, "popcount a-file");

  /* lsb_index / msb_index: zero -> -1; single bit -> its index; two bits. */
  QWC_EQ_I(lsb_index(0), -1, "lsb_index(0)");
  QWC_EQ_I(msb_index(0), -1, "msb_index(0)");
  QWC_EQ_I(lsb_index((u64)1), 0, "lsb bit0");
  QWC_EQ_I(msb_index((u64)1), 0, "msb bit0");
  QWC_EQ_I(lsb_index((u64)1 << 63), 63, "lsb bit63");
  QWC_EQ_I(msb_index((u64)1 << 63), 63, "msb bit63");
  QWC_EQ_I(lsb_index((u64)1 << 5 | (u64)1 << 3), 3, "lsb of two = lower");
  QWC_EQ_I(msb_index((u64)1 << 5 | (u64)1 << 3), 5, "msb of two = higher");

  /* pop_lsb: zero -> 0; single bit -> 0; two bits -> removes the lowest. */
  QWC_EQ_U64(pop_lsb(0), 0, "pop_lsb(0)");
  QWC_EQ_U64(pop_lsb((u64)1 << 3), 0, "pop_lsb single -> 0");
  QWC_EQ_U64(pop_lsb((u64)1 << 5 | (u64)1 << 3), (u64)1 << 5, "pop_lsb keeps higher");

  /* File edge masks: exactly the 8 squares of the file, no rank wrap. */
  QWC_EQ_U64(file_bb(0), 0x0101010101010101ULL, "file a mask");
  QWC_EQ_U64(file_bb(7), 0x8080808080808080ULL, "file h mask");
  for (int f = 0; f < FILE_NB; f++) {
    u64 expected = 0;
    for (int r = 0; r < RANK_NB; r++) expected |= (u64)1 << (r * 8 + f);
    QWC_EQ_U64(file_bb(f), expected, "file mask exact");
    QWC_EQ_U64(file_bb(f) & file_bb((f + 1) % 8), 0, "file mask no adjacent overlap");
  }
  QWC_EQ_U64(file_bb(-1), 0, "file -1 -> 0");
  QWC_EQ_U64(file_bb(8), 0, "file 8 -> 0");

  /* Rank edge masks: exactly the 8 squares of the rank, no file wrap. */
  QWC_EQ_U64(rank_bb(0), 0xFFULL, "rank 1 mask");
  QWC_EQ_U64(rank_bb(7), 0xFF00000000000000ULL, "rank 8 mask");
  for (int r = 0; r < RANK_NB; r++) {
    u64 expected = 0;
    for (int f = 0; f < FILE_NB; f++) expected |= (u64)1 << (r * 8 + f);
    QWC_EQ_U64(rank_bb(r), expected, "rank mask exact");
    QWC_EQ_U64(rank_bb(r) & rank_bb((r + 1) % 8), 0, "rank mask no adjacent overlap");
  }
  QWC_EQ_U64(rank_bb(-1), 0, "rank -1 -> 0");
  QWC_EQ_U64(rank_bb(8), 0, "rank 8 -> 0");

  return qwc_end();
}

int main(void) {
  int fail = 0;
  fail |= t_square();
  fail |= t_bitboard();
  return fail ? 1 : 0;
}
