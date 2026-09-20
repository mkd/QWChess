#include "core/attacks.h"
#include "core/square.h"

/* Eight directions. DIR_UP marks whether the direction's square indices
 * increase (lsb-trim) or decrease (msb-trim) relative to the origin. */
enum { DIR_N = 0, DIR_S, DIR_E, DIR_W, DIR_NE, DIR_NW, DIR_SE, DIR_SW, DIR_NB = 8 };
static const int DIR_DF[DIR_NB] = { 0, 0, 1, -1, 1, -1, 1, -1 };
static const int DIR_DR[DIR_NB] = { 1, -1, 0, 0, 1, 1, -1, -1 };
static const int DIR_UP[DIR_NB] = { 1, 0, 1, 0, 1, 1, 0, 0 };

static u64 g_rays[SQ_NB][DIR_NB];
static u64 g_pawn[2][SQ_NB];
static u64 g_knight[SQ_NB];
static u64 g_king[SQ_NB];
static int g_inited = 0;

static int onb(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

/* Walk a single direction's ray from the origin outward, emitting each square
 * until (and including) the first occupied square. `up` selects the traversal
 * order: least-significant first (indices increase) or most-significant first
 * (indices decrease), so the nearest blocker is always met first. */
static u64 trim_ray(u64 ray, u64 occ, int up) {
  u64 out = 0, r = ray;
  while (r) {
    int idx = up ? lsb_index(r) : msb_index(r);
    u64 bit = (u64)1 << idx;
    out |= bit;
    r ^= bit;              /* consume this square */
    if (occ & bit) break;  /* include the nearest blocker, stop */
  }
  return out;
}

void attacks_init(void) {
  for (int r = 0; r < 8; r++) {
    for (int f = 0; f < 8; f++) {
      int sq = (r << 3) | f;

      for (int d = 0; d < DIR_NB; d++) {
        u64 ray = 0;
        int nf = f + DIR_DF[d], nr = r + DIR_DR[d];
        while (onb(nf, nr)) { ray |= (u64)1 << ((nr << 3) | nf); nf += DIR_DF[d]; nr += DIR_DR[d]; }
        g_rays[sq][d] = ray;
      }

      for (int c = 0; c < 2; c++) {
        int dr = (c == WHITE) ? 1 : -1;
        u64 p = 0;
        for (int df = -1; df <= 1; df += 2) {
          int nf = f + df, nr = r + dr;
          if (onb(nf, nr)) p |= (u64)1 << ((nr << 3) | nf);
        }
        g_pawn[c][sq] = p;
      }

      { static const int kf[8] = { 1, 2, 2, 1, -1, -2, -2, -1 };
        static const int kr[8] = { 2, 1, -1, -2, -2, -1, 1, 2 };
        u64 k = 0;
        for (int i = 0; i < 8; i++) {
          int nf = f + kf[i], nr = r + kr[i];
          if (onb(nf, nr)) k |= (u64)1 << ((nr << 3) | nf);
        }
        g_knight[sq] = k; }

      { u64 k = 0;
        for (int df = -1; df <= 1; df++) {
          for (int dr = -1; dr <= 1; dr++) {
            if (df == 0 && dr == 0) continue;
            int nf = f + df, nr = r + dr;
            if (onb(nf, nr)) k |= (u64)1 << ((nr << 3) | nf);
          }
        }
        g_king[sq] = k; }
    }
  }
  g_inited = 1;
}

static void ensure(void) { if (!g_inited) attacks_init(); }

u64 pawn_attacks(Color c, Square sq)   { ensure(); return (sq < SQ_NB && c < 2) ? g_pawn[c][sq] : 0; }
u64 knight_attacks(Square sq)          { ensure(); return sq < SQ_NB ? g_knight[sq] : 0; }
u64 king_attacks(Square sq)            { ensure(); return sq < SQ_NB ? g_king[sq] : 0; }

u64 rook_attacks(Square sq, u64 occ) {
  ensure(); if (sq >= SQ_NB) return 0;
  return trim_ray(g_rays[sq][DIR_N], occ, DIR_UP[DIR_N]) |
         trim_ray(g_rays[sq][DIR_S], occ, DIR_UP[DIR_S]) |
         trim_ray(g_rays[sq][DIR_E], occ, DIR_UP[DIR_E]) |
         trim_ray(g_rays[sq][DIR_W], occ, DIR_UP[DIR_W]);
}

u64 bishop_attacks(Square sq, u64 occ) {
  ensure(); if (sq >= SQ_NB) return 0;
  return trim_ray(g_rays[sq][DIR_NE], occ, DIR_UP[DIR_NE]) |
         trim_ray(g_rays[sq][DIR_NW], occ, DIR_UP[DIR_NW]) |
         trim_ray(g_rays[sq][DIR_SE], occ, DIR_UP[DIR_SE]) |
         trim_ray(g_rays[sq][DIR_SW], occ, DIR_UP[DIR_SW]);
}

u64 queen_attacks(Square sq, u64 occ) { return rook_attacks(sq, occ) | bishop_attacks(sq, occ); }
