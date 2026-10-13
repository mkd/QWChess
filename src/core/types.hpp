/* QwenChess core foundational types (C++20).
 *
 * This header freezes the conventions every module agrees on. It is
 * self-contained (no project dependencies) and cheap to include. The board
 * model (Position), the undo record (StateInfo/MoveDelta) and the search
 * contract are defined in their own modules -- see docs/ARCHITECTURE.md.
 *
 * IMPORTANT: nothing NNUE/SFNNv16-specific belongs here. Chess-level types
 * only. The NNUE layer derives its own feature indices from these.
 */
#ifndef QWC_CORE_TYPES_HPP
#define QWC_CORE_TYPES_HPP

#include <cstdint>

/* ---- Fixed-width aliases ---- */
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

/* One score type everywhere. Side-to-move perspective. */
using Value = std::int32_t;

/* ---- Host width contract: QwenChess targets 64-bit x86_64 / arm64 ---- */
static_assert(sizeof(std::uintptr_t) == 8,
              "QwenChess requires a 64-bit host (uintptr_t must be 8 bytes)");
static_assert(sizeof(u64) == 8 && sizeof(u32) == 4 && sizeof(u16) == 2 && sizeof(u8) == 1,
              "fixed-width type sizes are wrong for this compiler");
static_assert(sizeof(Value) == 4, "Value must be a 32-bit signed integer");

/* ---- Board geometry ---- */
enum { FILE_A = 0, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H, FILE_NB = 8 };
enum { RANK_1 = 0, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8, RANK_NB = 8 };
enum { SQ_NB = 64 };

/* a1 = 0 ... h8 = 63 ; s = rank*8 + file (0-indexed, white viewpoint). */
using Square = u8;
enum SquareSentinel { NO_SQUARE = SQ_NB };   /* "no square" sentinel = 64 */

/* ---- Pieces. Values intentionally mirror Stockfish to ease later
 *      NNUE feature-index differential testing. CPP20-1 keeps the one-byte
 *      alias for the TYPE name and a separate plain enum for the VALUES,
 *      preserving every constant name, its value and the one-byte size.
 *      (Scoped enums with an explicit u8 base are introduced in CPP20-2.) ---- */
using PieceType = u8;
enum {
  NO_PIECE_TYPE = 0, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING,
  ALL_PIECES = 0, PIECE_TYPE_NB = 8
};
using Piece = u8;
enum {
  NO_PIECE = 0,
  W_PAWN = PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
  B_PAWN = PAWN + 8, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
  PIECE_NB = 16
};
using Color = u8;
enum { WHITE = 0, BLACK = 1, COLOR_NB = 2 };

/* Castling rights, one 4-bit u8 value. Each bit is an independent right; a
 * color loses a right when its king or the relevant rook moves, or when that
 * home square is captured. `CR_ALL` (15) is the orthodox starting position. */
enum {
  CR_WK = 1u << 0,   /* white may castle kingside (h1 rook present, king unmoved) */
  CR_WQ = 1u << 1,   /* white may castle queenside (a1 rook present) */
  CR_BK = 1u << 2,   /* black may castle kingside (h8 rook present) */
  CR_BQ = 1u << 3,   /* black may castle queenside (a8 rook present) */
  CR_ALL = CR_WK | CR_WQ | CR_BK | CR_BQ   /* == 15 */
};

static_assert(sizeof(PieceType) == 1 && sizeof(Piece) == 1 && sizeof(Color) == 1,
              "PieceType/Piece/Color must each be exactly one byte");
/* Pin the Stockfish-mirroring values that later drive NNUE feature indexing. */
static_assert(PAWN == 1 && QUEEN == 5 && KING == 6 && PIECE_TYPE_NB == 8,
              "piece-type values must match Stockfish");
static_assert(W_PAWN == 1 && W_KING == 6 && B_PAWN == 9 && B_KING == 14 && PIECE_NB == 16,
              "piece values must match Stockfish");
static_assert(WHITE == 0 && BLACK == 1 && COLOR_NB == 2, "color values must match Stockfish");

/* ---- Move: compact 32-bit encoding.
 *      [from:6][to:6][flags:3][reserved:1][promoType:3]  (bits 0..18 used).
 *      Value 0 is the reserved no/null move (from=to=0, no flags, no promo).
 *      from/to are 6-bit so a Move can never encode NO_SQUARE (64).
 *      Castling: from = king square, to = the rook's origin square.
 *      Promotion (MV_PROMO): promoType = target PieceType (KNIGHT..QUEEN = 2..5);
 *      a plain pawn push has no MV_PROMO and promoType 0.
 *      A double pawn push is NOT flag-encoded here; movegen derives it from a
 *      pawn moving two ranks (bit 15 is reserved for future flags). ---- */
using Move = u32;
enum MoveFlag {
  MV_EP = 1u << 0,        /* flags occupy bits 12..14 (3 bits) */
  MV_PROMO = 1u << 1,
  MV_CASTLE = 1u << 2,
};
static_assert(MV_EP == 1 && MV_PROMO == 2 && MV_CASTLE == 4, "move flag bit values");
/* Width guarantee (T002 group 3 inspection): the OR of every flag bit fits the
 * 3-bit flags field, and a plain `int` enum represents each value without
 * narrowing. No wider unsigned type is therefore required. */
static_assert((MV_EP | MV_PROMO | MV_CASTLE) <= 0x7, "MoveFlag values fit the 3-bit flags field");
inline Move make_move(Square from, Square to, u32 flags, u32 promoType) {
  u32 f = (u32)from & 0x3Fu;
  u32 t = (u32)to   & 0x3Fu;
  /* Mask flags to the 3 defined bits so a bad input cannot set the reserved
   * bit 15 or bleed into the promoType field (bits 16..18). */
  u32 fl = flags & 0x7u;
  u32 pr = promoType & 0x7u;
  return (Move)(f | (t << 6) | (fl << 12) | (pr << 16));
}
inline Square move_from(Move m) { return (Square)(m & 0x3F); }
inline Square move_to(Move m)   { return (Square)((m >> 6) & 0x3F); }
inline u32    move_flags(Move m){ return (m >> 12) & 0x7; }
inline u32    move_promo(Move m){ return (m >> 16) & 0x7; }
inline Move   move_none()       { return (Move)0; }
inline int    is_null_move(Move m) { return m == 0; }
inline int    is_ep(Move m)     { return (move_flags(m) & MV_EP) != 0; }
inline int    is_promotion(Move m) { return (move_flags(m) & MV_PROMO) != 0; }
inline int    is_castle(Move m) { return (move_flags(m) & MV_CASTLE) != 0; }
/* Representation validity: no bits in the reserved range 19..31 are set.
 * (from/to are inherently 6-bit; a Move cannot hold NO_SQUARE.) Semantic
 * legality (promo only with MV_PROMO, promo type 2..5, from != to for a real
 * move) is the movegen/Position concern, not the encoder's. */
inline int move_repr_is_valid(Move m) { return (m >> 19) == 0; }

/* ---- Search / history counters. Root-relative ply is DISTINCT from game
 *      history and the FEN fullmove number. Wide counters, validated at the
 *      Position boundary (do not use uint8_t: MAX_PLY=256 needs 0..256). ---- */
using SearchPly = u16;       /* root-relative depth, 0..MAX_PLY */
using HalfMoveClock = u16;   /* rule50 counter; validated [0,100], saturates */
using FullMoveNumber = u16;  /* FEN fullmove number, 1-based */

/* ---- One authoritative search limit (QwenChess's own).
 *      Stockfish's MAX_PLY=246 is an upstream reference fact only. QwenChess
 *      uses 256 and derives all mate/TB/stack bounds consistently from it. ---- */
enum { MAX_PLY = 256 };
enum { MAX_PLY_STACK = MAX_PLY + 1 };   /* root + MAX_PLY entries */
static_assert(MAX_PLY <= 0xFFFF, "MAX_PLY must fit a SearchPly (u16)");

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
static_assert(VALUE_MATED_IN_MAX_PLY < VALUE_TB_LOSS_IN_MAX_PLY, "score ordering");
static_assert(VALUE_TB_LOSS_IN_MAX_PLY < VALUE_DRAW, "score ordering");
static_assert(VALUE_DRAW < VALUE_TB_WIN_IN_MAX_PLY, "score ordering");
static_assert(VALUE_TB_WIN_IN_MAX_PLY < VALUE_MATE_IN_MAX_PLY, "score ordering");
static_assert(VALUE_MATE_IN_MAX_PLY < VALUE_INFINITE, "score ordering");
static_assert(VALUE_INFINITE < VALUE_NONE, "score ordering");

/* Mate distance helpers (side-to-move perspective). */
inline Value mate_in(int ply)  { return (Value)(VALUE_MATE - ply); }
inline Value mated_in(int ply) { return (Value)(-VALUE_MATE + ply); }
inline int is_mate(Value v)    { return v >= VALUE_MATE_IN_MAX_PLY; }

#endif /* QWC_CORE_TYPES_HPP */
