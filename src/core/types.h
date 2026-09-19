/* QwenChess core foundational types (C17).
 *
 * This header freezes the conventions every module agrees on. It is
 * self-contained (no project dependencies) and cheap to include. The board
 * model (Position), the undo record (StateInfo/MoveDelta) and the search
 * contract are defined in their own modules -- see docs/ARCHITECTURE.md.
 *
 * IMPORTANT: nothing NNUE/SFNNv16-specific belongs here. Chess-level types
 * only. The NNUE layer derives its own feature indices from these.
 */
#ifndef QWC_CORE_TYPES_H
#define QWC_CORE_TYPES_H

#include <stdint.h>

/* ---- Fixed-width aliases ---- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

/* One score type everywhere. Side-to-move perspective. */
typedef int32_t Value;

/* ---- Host width contract: QwenChess targets 64-bit x86_64 / arm64 ---- */
_Static_assert(sizeof(uintptr_t) == 8,
               "QwenChess requires a 64-bit host (uintptr_t must be 8 bytes)");
_Static_assert(sizeof(uint64_t) == 8 && sizeof(uint32_t) == 4 &&
               sizeof(uint16_t) == 2 && sizeof(uint8_t) == 1,
               "fixed-width type sizes are wrong for this compiler");
_Static_assert(sizeof(Value) == 4, "Value must be a 32-bit signed integer");

/* ---- Board geometry ---- */
enum { FILE_A = 0, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H, FILE_NB = 8 };
enum { RANK_1 = 0, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8, RANK_NB = 8 };
enum { SQ_NB = 64 };

/* a1 = 0 ... h8 = 63 ; s = rank*8 + file (0-indexed, white viewpoint). */
typedef u8 Square;
enum SquareSentinel { NO_SQUARE = SQ_NB };   /* "no square" sentinel = 64 */

/* ---- Pieces. Values intentionally mirror Stockfish to ease later
 *      NNUE feature-index differential testing. ---- */
enum PieceType : u8 {
  NO_PIECE_TYPE = 0, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING,
  ALL_PIECES = 0, PIECE_TYPE_NB = 8
};
enum Piece : u8 {
  NO_PIECE = 0,
  W_PAWN = PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
  B_PAWN = PAWN + 8, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
  PIECE_NB = 16
};
enum Color : u8 { WHITE = 0, BLACK = 1, COLOR_NB = 2 };

/* ---- Move: compact 32-bit encoding.
 *      [from:6][to:6][flags:4][promoType:3]  (19 bits used, 0 = no move)
 *      Refined in the movegen task; accessors are stable. ---- */
typedef u32 Move;
enum MoveFlag : u32 {
  MV_EP = 1u << 0,       /* flags bits */
  MV_PROMO = 1u << 1,
  MV_CASTLE = 1u << 2,
};
static inline Move make_move(Square from, Square to, u32 flags, u32 promoType) {
  u32 f = (u32)from & 0x3Fu;
  u32 t = (u32)to   & 0x3Fu;
  return (Move)(f | (t << 6) | (flags << 12) | (promoType << 16));
}
static inline Square     move_from(Move m) { return (Square)(m & 0x3F); }
static inline Square     move_to(Move m)   { return (Square)((m >> 6) & 0x3F); }
static inline u32        move_flags(Move m){ return (m >> 12) & 0x7; }
static inline u32        move_promo(Move m){ return (m >> 16) & 0x7; }
static inline Move       move_none(void)   { return (Move)0; }
static inline int        is_null_move(Move m) { return m == 0; }

/* ---- Search / history counters. Root-relative ply is DISTINCT from game
 *      history and the FEN fullmove number. Wide counters, validated at the
 *      Position boundary (do not use uint8_t: MAX_PLY=256 needs 0..256). ---- */
typedef u16 SearchPly;       /* root-relative depth, 0..MAX_PLY */
typedef u16 HalfMoveClock;   /* rule50 counter; validated [0,100], saturates */
typedef u16 FullMoveNumber;  /* FEN fullmove number, 1-based */

/* ---- One authoritative search limit (QwenChess's own).
 *      Stockfish's MAX_PLY=246 is an upstream reference fact only. QwenChess
 *      uses 256 and derives all mate/TB/stack bounds consistently from it. ---- */
enum { MAX_PLY = 256 };
enum { MAX_PLY_STACK = MAX_PLY + 1 };   /* root + MAX_PLY entries */
_Static_assert(MAX_PLY <= 0xFFFF, "MAX_PLY must fit a SearchPly (u16)");

/* ---- Score scale + reserved ranges, all derived from MAX_PLY.
 *      Ordering (low..high): mated_in_max < tb_loss < draw(0) < tb_win <
 *      mate_in_max < infinite < none. NNUE static eval is clamped to the open
 *      interval (tb_loss, tb_win) so it never collides with mate/TB scores. ---- */
enum {
  VALUE_ZERO     = 0,
  VALUE_DRAW     = 0,
  VALUE_MATE     = 31000,
  VALUE_MATE_IN_MAX_PLY  = VALUE_MATE - MAX_PLY,
  VALUE_MATED_IN_MAX_PLY = -VALUE_MATE_IN_MAX_PLY,
  VALUE_TB       = VALUE_MATE_IN_MAX_PLY - 1,
  VALUE_TB_WIN_IN_MAX_PLY  = VALUE_TB - MAX_PLY,
  VALUE_TB_LOSS_IN_MAX_PLY = -VALUE_TB_WIN_IN_MAX_PLY,
  VALUE_INFINITE = 32000,
  VALUE_NONE     = 32001,   /* "no value" sentinel */
};
_Static_assert(VALUE_MATED_IN_MAX_PLY < VALUE_TB_LOSS_IN_MAX_PLY, "score ordering");
_Static_assert(VALUE_TB_LOSS_IN_MAX_PLY < VALUE_DRAW, "score ordering");
_Static_assert(VALUE_DRAW < VALUE_TB_WIN_IN_MAX_PLY, "score ordering");
_Static_assert(VALUE_TB_WIN_IN_MAX_PLY < VALUE_MATE_IN_MAX_PLY, "score ordering");
_Static_assert(VALUE_MATE_IN_MAX_PLY < VALUE_INFINITE, "score ordering");
_Static_assert(VALUE_INFINITE < VALUE_NONE, "score ordering");

/* Mate distance helpers (side-to-move perspective). */
static inline Value mate_in(int ply)  { return (Value)(VALUE_MATE - ply); }
static inline Value mated_in(int ply) { return (Value)(-VALUE_MATE + ply); }
static inline int is_mate(Value v)    { return v >= VALUE_MATE_IN_MAX_PLY; }

#endif /* QWC_CORE_TYPES_H */
