/* QwenChess scaffold test (T001).
 *
 * Covers the behaviour actually implemented in the scaffold:
 *   - compile-time: fixed-width / host / encoding / score-range invariants
 *   - runtime: move-encoding round-trip
 *   - runtime: the monotonic clock is nondecreasing (no positive-elapsed
 *     requirement, no long sleep)
 *
 * Exits 0 on full pass, 1 otherwise. Runs identically under ASan/UBSan. */
#include <cstdio>
#include <cstdint>
#include "core/types.hpp"
#include "platform/clock.hpp"

/* Compile-time invariants (duplicating the ones in types.hpp so this test is
 * self-contained about the conventions it relies on). */
static_assert(sizeof(Value) == 4, "Value is int32_t");
static_assert(NO_SQUARE == 64, "no-square sentinel is 64");
static_assert(PIECE_NB == 16, "16 piece codes");
static_assert(PIECE_TYPE_NB == 8, "8 piece types");
static_assert(MAX_PLY_STACK == MAX_PLY + 1, "stack holds root + MAX_PLY");
static_assert(MAX_PLY <= 0xFFFF, "MAX_PLY fits SearchPly (u16)");
static_assert(to_under(Piece::B_PAWN) == to_under(Piece::W_PAWN) + 8, "piece colour offset");

/* Runtime: compact move encoding round-trips all fields. */
static int test_move_roundtrip(void) {
  Move castle = make_move(4, 3, to_under(MoveFlag::MV_CASTLE), 0);  /* e1-h1 style (a1->d1) */
  if (move_from(castle) != 4 || move_to(castle) != 3 ||
      move_flags(castle) != to_under(MoveFlag::MV_CASTLE) || move_promo(castle) != 0)
    return 0;

  Move promo = make_move(63, 55, to_under(MoveFlag::MV_PROMO), to_under(PieceType::QUEEN));
  if (move_from(promo) != 63 || move_to(promo) != 55 ||
      move_flags(promo) != to_under(MoveFlag::MV_PROMO) || move_promo(promo) != to_under(PieceType::QUEEN))
    return 0;

  Move ep = make_move(35, 28, to_under(MoveFlag::MV_EP), 0);
  if (move_from(ep) != 35 || move_to(ep) != 28 || move_flags(ep) != to_under(MoveFlag::MV_EP))
    return 0;

  if (!is_null_move(move_none()))
    return 0;
  return 1;
}

/* Runtime: the clock is monotonic (nondecreasing). ~1e5 vDSO reads, a few ms. */
static int test_clock_monotonic(void) {
  i64 prev = qwc_clock_now_ns();
  if (prev < 0)
    return 0; /* clock error */
  for (int i = 0; i < 100000; i++) {
    i64 t = qwc_clock_now_ns();
    if (t < 0 || t < prev)
      return 0; /* regression */
    prev = t;
  }
  i64 us = qwc_clock_now_us();
  return us >= 0;
}

int main(void) {
  int ok = 1;
  ok &= test_move_roundtrip();
  ok &= test_clock_monotonic();
  std::printf("scaffold test: %s (move_roundtrip, clock_monotonic)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
