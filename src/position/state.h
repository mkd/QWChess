/* QwenChess: reversible move application (make/unmake) — the undo record.
 *
 * This is T004 checkpoint 1: the StateInfo/MoveDelta undo architecture and the
 * reversible application of ORDINARY moves (quiet moves, ordinary captures,
 * pawn single/double pushes, ordinary pawn captures). En-passant captures,
 * promotions/underpromotions, castling and null moves are LATER T004
 * checkpoints; they are reserved in the delta's capacity and are REJECTED
 * cleanly here.
 *
 * Chess-level only: no NNUE/SFNNv16 types, feature indices or evaluator
 * pointers (see docs/ARCHITECTURE.md, "The reversible-move contract"). A future
 * NNUE consumer reads the chess-level MoveDelta (the per-square before/after
 * pieces) + the Position and derives its own feature-index deltas; it never owns
 * a board.
 *
 * Model: ONE mutable Position per worker. The caller supplies a distinct
 * StateInfo undo record for each outstanding move and undoes in strict LIFO
 * order (the last made is the first unmaked). No heap allocation and no
 * whole-Position snapshot is taken per ply; the record holds a bounded piece
 * delta plus the previous scalar state. The record is caller-owned and must be
 * at least as large as the outstanding-move stack (a worker/stack framework
 * lands later; pre-root game history remains independent of MAX_PLY).
 */
#ifndef QWC_POSITION_STATE_H
#define QWC_POSITION_STATE_H

#include "core/types.h"

/* `Position` is defined in position/position.h, which includes this header (it
 * is the natural home of the make/unmake API). To avoid a hard include cycle we
 * forward-declare the tag here; the prototypes take `struct Position *`, which is
 * the same type. A file that needs the full Position definition includes
 * position/position.h (which brings in both). */
struct Position;

/* One board-square change: the square and its piece before and after the move.
 * NO_PIECE in a field means that square was / is empty. */
typedef struct SquareEdit {
  Square  sq;     /* the square that changes (0..63) */
  Piece   before; /* piece occupying sq before the move (NO_PIECE if empty) */
  Piece   after;  /* piece occupying sq after  the move (NO_PIECE if empty) */
} SquareEdit;

/* Bounded capacity. 2 edits cover every move supported by checkpoint 1
 * (origin loses the piece, destination gains it). 3 covers a future en-passant
 * capture (captor origin / target landing / captured pawn) and 4 covers
 * orthodox castling (king origin/dest + rook origin/dest). */
enum { MAX_EDIT = 4 };

/* The reversible board change of one move: the applied Move plus the populated
 * square edits. The edits are sufficient for a future NNUE consumer to rebuild
 * the board difference without a second authoritative board. */
typedef struct MoveDelta {
  Move       move;         /* the applied move (from/to/flags/promo) */
  int        edit_count;   /* number of populated entries in `edits` (<= MAX_EDIT) */
  SquareEdit edits[MAX_EDIT];
} MoveDelta;

/* The undo record for one applied move. `delta` reverses the board; the prev_*
 * scalars restore the irreversible metadata exactly. Full-width u16 counters
 * (no narrowing; the full rule-50 / fullmove range is retained). */
typedef struct StateInfo {
  MoveDelta  delta;        /* reversible piece changes */
  u64        prev_key;     /* position key before the move */
  u8         prev_side;    /* side to move before */
  u8         prev_cr;      /* castling rights before */
  Square     prev_ep;      /* exact RECORDED ep square before, or NO_SQUARE */
  u16        prev_halfmove;/* rule-50 clock before (full width) */
  u16        prev_fullmove;/* fullmove number before (full width) */
} StateInfo;

/* Pseudo-legal move application on an initialized, consistent Position.
 *
 * Applies `m` to *pos and records the undo state in *state. Returns 1 on
 * success. On any failure (malformed encoding, a move kind checkpoint 1 does
 * not support, invalid piece geometry/path, wrong source ownership, an
 * illegal destination, or a counter that would overflow) it returns 0 and
 * leaves BOTH *pos and *state byte-for-byte unchanged: every input is validated
 * before any field is written.
 *
 * Supported here: quiet moves, ordinary captures, pawn single/double pushes,
 * ordinary pawn captures. A Move whose from/to are valid 6-bit squares is
 * accepted by shape; semantic pseudo-legality (own-king safety, check) is NOT
 * a precondition — that is movegen's job in T005. A successful application is
 * therefore not a legality certificate. The caller must supply a valid,
 * consistent Position (pos_validate) and a distinct StateInfo. */
int pos_make_move(struct Position *pos, StateInfo *state, Move m);

/* Reverse the move recorded in *state, restoring *pos's entire logical state
 * (mailbox, byPiece, byColor, occupancy, king squares, side, castling rights,
 * the recorded ep, both counters and the key) exactly to what it was before the
 * matching pos_make_move. *state is not modified and may be reused for the next
 * move. Must be called in LIFO order for a valid, consistent *pos. */
void pos_unmake_move(struct Position *pos, StateInfo *state);

#endif /* QWC_POSITION_STATE_H */
