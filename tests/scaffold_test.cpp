/* QwenChess scaffold test (T001).
 *
 * Covers the behaviour actually implemented in the scaffold:
 *   - compile-time: fixed-width / host / encoding / score-range invariants
 *   - compile-time: the domain enums are the exact scoped types (u8/u32 base, no
 *     implicit integer conversion) with every value/sentinel frozen
 *   - compile-time: Position/StateInfo/MoveDelta/SquareEdit are trivially
 *     copyable + standard-layout, and the x86-64 size/alignment baseline holds
 *   - runtime: move-encoding round-trip
 *   - runtime: the monotonic clock is nondecreasing (no positive-elapsed
 *     requirement, no long sleep)
 *
 * Exits 0 on full pass, 1 otherwise. Runs identically under ASan/UBSan.
 *
 * The compile-time assertions below include the chess-level aggregate layouts
 * (Position/StateInfo/MoveDelta/SquareEdit), so this test also includes the
 * position/state headers. Those types are used only for sizeof/alignof and the
 * <type_traits> below (all evaluated at compile time), so no position/state
 * symbols are referenced and the test's link set is unchanged. */
#include <cstdio>
#include <cstdint>
#include <type_traits>
#include <array>
#include <algorithm>
#include "core/types.hpp"
#include "platform/clock.hpp"
#include "position/position.hpp"   // Position; transitively state.hpp (StateInfo/MoveDelta/SquareEdit)

using namespace qwc;

/* Compile-time invariants (duplicating the ones in types.hpp so this test is
 * self-contained about the conventions it relies on). */
static_assert(sizeof(Value) == 4, "Value is int32_t");
static_assert(NO_SQUARE == 64, "no-square sentinel is 64");
static_assert(PIECE_NB == 16, "16 piece codes");
static_assert(PIECE_TYPE_NB == 8, "8 piece types");
static_assert(MAX_PLY_STACK == MAX_PLY + 1, "stack holds root + MAX_PLY");
static_assert(MAX_PLY <= 0xFFFF, "MAX_PLY fits SearchPly (u16)");
static_assert(to_under(Piece::B_PAWN) == to_under(Piece::W_PAWN) + 8, "piece colour offset");

/* ---- Compile-time: the four domain types are the exact requested types. ----
 * std::is_enum_v only proves "is an enum"; it does NOT prove a scoped enum. A
 * scoped enum has no implicit conversion to its base or to int, whereas a plain
 * enum does (integral promotion). So we pin BOTH the exact underlying type and
 * the absence of implicit integer conversion to establish scoping. */
static_assert(std::is_enum_v<PieceType> && !std::is_convertible_v<PieceType, int>
              && !std::is_convertible_v<PieceType, u8>
              && std::is_same_v<std::underlying_type_t<PieceType>, u8>,
              "PieceType is a scoped enum with a u8 base");
static_assert(std::is_enum_v<Piece> && !std::is_convertible_v<Piece, int>
              && !std::is_convertible_v<Piece, u8>
              && std::is_same_v<std::underlying_type_t<Piece>, u8>,
              "Piece is a scoped enum with a u8 base");
static_assert(std::is_enum_v<Color> && !std::is_convertible_v<Color, int>
              && !std::is_convertible_v<Color, u8>
              && std::is_same_v<std::underlying_type_t<Color>, u8>,
              "Color is a scoped enum with a u8 base");
static_assert(std::is_enum_v<MoveFlag> && !std::is_convertible_v<MoveFlag, int>
              && !std::is_convertible_v<MoveFlag, u32>
              && std::is_same_v<std::underlying_type_t<MoveFlag>, u32>,
              "MoveFlag is a scoped enum with a u32 base");

/* ---- Compile-time: freeze every named compatibility value + sentinel. These
 * mirror Stockfish and drive later NNUE feature indexing, so each is pinned to
 * its independent numeric expectation (not just one representative). ---- */
static_assert(to_under(PieceType::NO_PIECE_TYPE) == 0 && to_under(PieceType::PAWN) == 1
              && to_under(PieceType::KNIGHT) == 2 && to_under(PieceType::BISHOP) == 3
              && to_under(PieceType::ROOK) == 4 && to_under(PieceType::QUEEN) == 5
              && to_under(PieceType::KING) == 6 && to_under(PieceType::ALL_PIECES) == 0,
              "all PieceType codes are frozen");
static_assert(to_under(Piece::NO_PIECE) == 0
              && to_under(Piece::W_PAWN) == 1 && to_under(Piece::W_KNIGHT) == 2
              && to_under(Piece::W_BISHOP) == 3 && to_under(Piece::W_ROOK) == 4
              && to_under(Piece::W_QUEEN) == 5 && to_under(Piece::W_KING) == 6
              && to_under(Piece::B_PAWN) == 9 && to_under(Piece::B_KNIGHT) == 10
              && to_under(Piece::B_BISHOP) == 11 && to_under(Piece::B_ROOK) == 12
              && to_under(Piece::B_QUEEN) == 13 && to_under(Piece::B_KING) == 14,
              "all Piece codes are frozen");
static_assert(to_under(Color::WHITE) == 0 && to_under(Color::BLACK) == 1,
              "both Color codes are frozen");
static_assert(to_under(MoveFlag::NO_FLAGS) == 0 && to_under(MoveFlag::MV_EP) == 1
              && to_under(MoveFlag::MV_PROMO) == 2 && to_under(MoveFlag::MV_CASTLE) == 4,
              "all MoveFlag bit values are frozen");

/* ---- Compile-time: the chess-level aggregates remain plain C aggregates --
 * trivially copyable and standard-layout (no user-declared special members, no
 * virtuals). These are ABI-independent properties. ---- */
static_assert(std::is_trivially_copyable_v<Position> && std::is_standard_layout_v<Position>,
              "Position is trivially copyable and standard-layout");
static_assert(std::is_trivially_copyable_v<StateInfo> && std::is_standard_layout_v<StateInfo>,
              "StateInfo is trivially copyable and standard-layout");
static_assert(std::is_trivially_copyable_v<MoveDelta> && std::is_standard_layout_v<MoveDelta>,
              "MoveDelta is trivially copyable and standard-layout");
static_assert(std::is_trivially_copyable_v<SquareEdit> && std::is_standard_layout_v<SquareEdit>,
              "SquareEdit is trivially copyable and standard-layout");

/* ---- Compile-time: the aggregate size/alignment baseline, pinned to the ABI
 * where it was measured (x86-64: 248/8, 48/8, 20/4, 3/1). A different ABI keeps
 * the copyable/layout guarantees above but is NOT pinned to these sizes. ---- */
#if defined(__x86_64__) || defined(_M_X64)
static_assert(sizeof(Position) == 248 && alignof(Position) == 8, "x86-64: Position is 248/8");
static_assert(sizeof(StateInfo) == 48 && alignof(StateInfo) == 8, "x86-64: StateInfo is 48/8");
static_assert(sizeof(MoveDelta) == 20 && alignof(MoveDelta) == 4, "x86-64: MoveDelta is 20/4");
static_assert(sizeof(SquareEdit) == 3 && alignof(SquareEdit) == 1, "x86-64: SquareEdit is 3/1");
#endif

/* Runtime: compact move encoding round-trips all fields. */
static int test_move_roundtrip() {
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
static int test_clock_monotonic() {
  i64 prev = clock_now_ns();
  if (prev < 0)
    return 0; /* clock error */
  for (int i = 0; i < 100000; i++) {
    i64 t = clock_now_ns();
    if (t < 0 || t < prev)
      return 0; /* regression */
    prev = t;
  }
  i64 us = clock_now_us();
  return us >= 0;
}

int main() {
  int ok = 1;
  ok &= test_move_roundtrip();
  ok &= test_clock_monotonic();
  std::printf("scaffold test: %s (move_roundtrip, clock_monotonic)\n",
         ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
