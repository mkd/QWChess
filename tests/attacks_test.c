/* QwenChess T002 test -- group 3: independent verification of attack generation.
 *
 * The reference below is deliberately independent of the production code in
 * attacks.c: it recomputes file/rank from the square index with its own bit
 * math and steps ONE square per direction to the board edge (emitting each
 * square, including the first occupied one, then stopping). It does NOT call
 * the production attack functions, reuse the production rays, direction tables
 * or nearest-blocker trimming. Manual corner/edge fixtures pin known-correct
 * sets so a reference and production that were wrong the same way cannot both
 * pass. Board legality (e.g. which side a pawn stands on) is out of scope.
 *
 * Group coverage:
 *   pawn/knight/king : hand fixtures + every square vs the reference
 *   rook/bishop      : EVERY subset of each square's ray mask (exhaustive)
 *   queen            : 128 seeded full-board occupancies per square
 */
#include <time.h>
#include "core/types.h"
#include "core/bitboard.h"
#include "core/attacks.h"
#include "test_util.h"

/* ---------- independent reference (own coordinate stepping, no production) --- */
static int  ref_onb(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }
static u64  ref_bit(int f, int r) { return (u64)1 << ((r << 3) | f); }

/* Step (df,dr) one square at a time: emit each square reached, including the
 * first occupied one, then stop. The source square is never emitted. */
static u64 ref_ray(int f, int r, int df, int dr, u64 occ) {
  u64 out = 0;
  int nf = f + df, nr = r + dr;
  while (ref_onb(nf, nr)) {
    u64 b = ref_bit(nf, nr);
    out |= b;
    if (occ & b) break;
    nf += df; nr += dr;
  }
  return out;
}
static u64 ref_rook(int sq, u64 occ) {
  int f = sq & 7, r = sq >> 3;
  return ref_ray(f, r, 0, 1, occ) | ref_ray(f, r, 0, -1, occ) |
         ref_ray(f, r, 1, 0, occ) | ref_ray(f, r, -1, 0, occ);
}
static u64 ref_bishop(int sq, u64 occ) {
  int f = sq & 7, r = sq >> 3;
  return ref_ray(f, r, 1, 1, occ) | ref_ray(f, r, -1, 1, occ) |
         ref_ray(f, r, 1, -1, occ) | ref_ray(f, r, -1, -1, occ);
}
/* Full ray masks: occ=0 means no blocker, so the whole ray to the board edge is
 * emitted (source excluded, terminal edge squares included). Used to enumerate
 * exhaustive occupancies in Phase 2. */
static u64 ref_rook_mask(int sq) {
  int f = sq & 7, r = sq >> 3;
  return ref_ray(f, r, 0, 1, 0) | ref_ray(f, r, 0, -1, 0) |
         ref_ray(f, r, 1, 0, 0) | ref_ray(f, r, -1, 0, 0);
}
static u64 ref_bishop_mask(int sq) {
  int f = sq & 7, r = sq >> 3;
  return ref_ray(f, r, 1, 1, 0) | ref_ray(f, r, -1, 1, 0) |
         ref_ray(f, r, 1, -1, 0) | ref_ray(f, r, -1, -1, 0);
}
static u64 ref_queen(int sq, u64 occ) { return ref_rook(sq, occ) | ref_bishop(sq, occ); }

static u64 ref_pawn(Color c, int sq) {
  int f = sq & 7, r = sq >> 3, dr = (c == WHITE) ? 1 : -1;
  u64 p = 0;
  if (ref_onb(f - 1, r + dr)) p |= ref_bit(f - 1, r + dr);
  if (ref_onb(f + 1, r + dr)) p |= ref_bit(f + 1, r + dr);
  return p;
}
static u64 ref_knight(int sq) {
  static const int kf[8] = { 1, 2, 2, 1, -1, -2, -2, -1 };
  static const int kr[8] = { 2, 1, -1, -2, -2, -1, 1, 2 };
  int f = sq & 7, r = sq >> 3;
  u64 k = 0;
  for (int i = 0; i < 8; i++) {
    int nf = f + kf[i], nr = r + kr[i];
    if (ref_onb(nf, nr)) k |= ref_bit(nf, nr);
  }
  return k;
}
static u64 ref_king(int sq) {
  int f = sq & 7, r = sq >> 3;
  u64 k = 0;
  for (int df = -1; df <= 1; df++)
    for (int dr = -1; dr <= 1; dr++) {
      if (df == 0 && dr == 0) continue;
      int nf = f + df, nr = r + dr;
      if (ref_onb(nf, nr)) k |= ref_bit(nf, nr);
    }
  return k;
}

/* Count one comparison; on mismatch print the full context (bounded so a
 * systematic failure cannot flood the output). Reuses the harness counters so
 * qwc_end() reports the aggregate. */
static void cmp_bb(const char *what, Color c, int sq, u64 occ, u64 got, u64 exp) {
  qwc_cases++;
  if (got != exp) {
    qwc_fails++;
    if (qwc_fails <= qwc_fail_budget)
      printf("  FAIL %s color=%d sq=%d occ=0x%016llx got=0x%016llx exp=0x%016llx\n",
             what, (int)c, sq, (unsigned long long)occ,
             (unsigned long long)got, (unsigned long long)exp);
  }
}

/* ---------- Phase 1: pawn / knight / king ---------- */
static int t_pawn(void) {
  qwc_begin("attacks.pawn");
  /* reference must reproduce known-correct capture sets (independent of prod) */
  cmp_bb("pawn.ref", WHITE, 0,  0, ref_pawn(WHITE, 0),  ref_bit(1, 1));  /* a1 -> b2 */
  cmp_bb("pawn.ref", WHITE, 55, 0, ref_pawn(WHITE, 55), ref_bit(6, 7));  /* h7 -> g8 */
  cmp_bb("pawn.ref", BLACK, 63, 0, ref_pawn(BLACK, 63), ref_bit(6, 6));  /* h8 -> g7 */
  cmp_bb("pawn.ref", BLACK, 56, 0, ref_pawn(BLACK, 56), ref_bit(1, 6));  /* a8 -> b7 */
  /* production must match the reference on every square / color */
  for (int c = 0; c < 2; c++)
    for (int sq = 0; sq < SQ_NB; sq++)
      cmp_bb("pawn", (Color)c, sq, 0, pawn_attacks((Color)c, (Square)sq), ref_pawn((Color)c, sq));
  return qwc_end();
}

static int t_knight(void) {
  qwc_begin("attacks.knight");
  /* a1 corner = 2 moves (b3,c2); e4 center = 8 moves */
  cmp_bb("knight.ref", WHITE, 0, 0, ref_knight(0),
         ref_bit(1, 2) | ref_bit(2, 1));
  u64 e4 = ref_bit(3, 5) | ref_bit(5, 5) | ref_bit(6, 4) | ref_bit(6, 2) |
           ref_bit(5, 1) | ref_bit(3, 1) | ref_bit(2, 2) | ref_bit(2, 4);
  cmp_bb("knight.ref", WHITE, 28, 0, ref_knight(28), e4);
  for (int sq = 0; sq < SQ_NB; sq++)
    cmp_bb("knight", WHITE, sq, 0, knight_attacks((Square)sq), ref_knight(sq));
  return qwc_end();
}

static int t_king(void) {
  qwc_begin("attacks.king");
  cmp_bb("king.ref", WHITE, 0,  0, ref_king(0),  ref_bit(1, 0) | ref_bit(0, 1) | ref_bit(1, 1));  /* a1 */
  cmp_bb("king.ref", WHITE, 63, 0, ref_king(63), ref_bit(6, 7) | ref_bit(7, 6) | ref_bit(6, 6));  /* h8 */
  for (int sq = 0; sq < SQ_NB; sq++)
    cmp_bb("king", WHITE, sq, 0, king_attacks((Square)sq), ref_king(sq));
  return qwc_end();
}

/* ---------- Phase 2: exhaustive slider subsets ----------
 * For each square, enumerate EVERY subset of its (independent) ray mask as an
 * occupancy and compare the production attack against the coordinate reference.
 * Off-ray occupancy bits never affect a slider, so this is complete coverage of
 * the trim-at-first-blocker rule. */
static int t_rook_exhaustive(void) {
  qwc_begin("attacks.rook.exhaustive");
  long total = 0;
  for (int sq = 0; sq < SQ_NB; sq++) {
    u64 mask = ref_rook_mask(sq);
    for (u64 s = mask; ; s = (s - 1) & mask) {
      cmp_bb("rook", WHITE, sq, s, rook_attacks((Square)sq, s), ref_rook(sq, s));
      total++;
      if (s == 0) break;
    }
  }
  QWC_EQ_U64((u64)total, 1048576ull, "rook enumeration count");
  return qwc_end();
}
static int t_bishop_exhaustive(void) {
  qwc_begin("attacks.bishop.exhaustive");
  long total = 0;
  for (int sq = 0; sq < SQ_NB; sq++) {
    u64 mask = ref_bishop_mask(sq);
    for (u64 s = mask; ; s = (s - 1) & mask) {
      cmp_bb("bishop", WHITE, sq, s, bishop_attacks((Square)sq, s), ref_bishop(sq, s));
      total++;
      if (s == 0) break;
    }
  }
  QWC_EQ_U64((u64)total, 71168ull, "bishop enumeration count");
  return qwc_end();
}

/* ---------- Phase 3: source invariance + seeded full-board queen ---------- */

/* Deterministic test-only PRNG (splitmix64). Deliberately independent of the
 * production Zobrist module (a different, still-unverified hashing PRNG), so the
 * generated occupancies never depend on that module. All unsigned ops wrap
 * well-definedly -- no UB. */
static u64 prng_state;
static u64 prng_next(void) {
  prng_state += 0x9E3779B97F4A7C15ull;
  u64 z = prng_state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/* The square's own occupancy bit must not change any slider attack (the ray
 * excludes the source), and the source-free occupancy must match the reference. */
static int t_source_invariance(void) {
  qwc_begin("attacks.source.invariance");
  for (int sq = 0; sq < SQ_NB; sq++) {
    u64 src = (u64)1 << sq;
    u64 occs[2] = { 0, BB_FULL };
    for (int k = 0; k < 2; k++) {
      u64 o = occs[k];
      QWC_TRUE(rook_attacks((Square)sq, o)   == rook_attacks((Square)sq, o & ~src), "rook source-invariant");
      QWC_TRUE(bishop_attacks((Square)sq, o) == bishop_attacks((Square)sq, o & ~src), "bishop source-invariant");
      QWC_TRUE(queen_attacks((Square)sq, o)  == queen_attacks((Square)sq, o & ~src), "queen source-invariant");
      cmp_bb("rook", WHITE, sq, o & ~src, rook_attacks((Square)sq, o & ~src), ref_rook(sq, o & ~src));
    }
  }
  return qwc_end();
}

/* 128 seeded full-board occupancies per square: random off-ray noise plus real
 * on-ray blockers, comparing the production queen against the reference union. */
static int t_queen_seeded(void) {
  prng_state = 0x20260920ull;
  qwc_begin("attacks.queen.seeded");
  printf("  (seed 0x20260920; 128 full-board occ/square x 64 squares)\n");
  for (int sq = 0; sq < SQ_NB; sq++)
    for (int i = 0; i < 128; i++) {
      u64 occ = prng_next();
      cmp_bb("queen", WHITE, sq, occ, queen_attacks((Square)sq, occ), ref_queen(sq, occ));
    }
  return qwc_end();
}

int main(void) {
  attacks_init();
  clock_t t0 = clock();
  int fail = 0;
  fail |= t_pawn();
  fail |= t_knight();
  fail |= t_king();
  fail |= t_rook_exhaustive();
  fail |= t_bishop_exhaustive();
  fail |= t_source_invariance();
  fail |= t_queen_seeded();
  printf("attacks total %.3fs -> %s\n",
         (double)(clock() - t0) / CLOCKS_PER_SEC, fail ? "FAIL" : "PASS");
  return fail ? 1 : 0;
}
