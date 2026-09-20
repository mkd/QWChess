#include "core/zobrist.h"

/* SplitMix64 (documented fixed-seed PRNG). Advances *state in place and
 * returns the next 64-bit value. Standard SplitMix64 mix constants. */
static u64 splitmix64(u64 *state) {
  u64 z = (*state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

/* Documented fixed seed (the 64-bit golden-ratio constant). */
#define ZOB_SEED 0x9E3779B97F4A7C15ULL

/* Table generation order (deterministic):
 *   1. piece-square keys: for rank 0..7, file 0..7, piece 0..15  (square-major)
 *   2. the black side-to-move key (1 draw); the white key is fixed to 0
 *   3. castling-rights keys: rights 0..15
 *   4. en-passant-file keys: file 0..7
 */
static u64 g_piece[PIECE_NB][SQ_NB];
static u64 g_side[COLOR_NB];
static u64 g_castling[16];
static u64 g_epfile[FILE_NB];
static int g_inited = 0;

void zobrist_init(void) {
  u64 st = ZOB_SEED;
  for (int r = 0; r < RANK_NB; r++)
    for (int f = 0; f < FILE_NB; f++)
      for (int p = 0; p < PIECE_NB; p++)
        g_piece[p][(r << 3) | f] = splitmix64(&st);

  g_side[WHITE] = 0;             /* white to move contributes the identity */
  g_side[BLACK] = splitmix64(&st);

  for (int i = 0; i < 16; i++) g_castling[i] = splitmix64(&st);
  for (int f = 0; f < FILE_NB; f++) g_epfile[f] = splitmix64(&st);

  g_inited = 1;
}

static void ensure(void) { if (!g_inited) zobrist_init(); }

u64 zobrist_piece(Piece p, Square sq) {
  ensure();
  return (p < PIECE_NB && sq < SQ_NB) ? g_piece[p][sq] : 0;
}
u64 zobrist_side(Color c) {
  ensure();
  return (c < COLOR_NB) ? g_side[c] : 0;   /* WHITE -> 0, BLACK -> key */
}
u64 zobrist_castling(u8 rights) { ensure(); return g_castling[rights & 0xF]; }
u64 zobrist_ep_file(int file) {
  ensure();
  return (file >= 0 && file < FILE_NB) ? g_epfile[file] : 0;
}

u64 zobrist_compute(const u8 mailbox[64], Color side, u8 rights, int ep_file) {
  ensure();
  u64 h = 0;
  for (int s = 0; s < SQ_NB; s++) {
    int p = mailbox[s];
    if (p) h ^= g_piece[p][s];
  }
  h ^= (side < COLOR_NB) ? g_side[side] : 0;
  h ^= g_castling[rights & 0xF];
  if (ep_file >= 0 && ep_file < FILE_NB) h ^= g_epfile[ep_file];
  return h;
}
