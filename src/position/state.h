/* QwenChess: reversible move application (make/unmake) — the undo record.
 *
 * This is T004 checkpoint 1+2+3+4: the StateInfo/MoveDelta undo architecture
 * and the reversible application of ORDINARY moves (checkpoint 1: quiet moves,
 * ordinary captures, pawn single/double pushes, ordinary pawn captures),
 * EN-PASSANT captures (checkpoint 2; the 3-edit application), orthodox
 * CASTLING (checkpoint 3; the 4-edit application) and PROMOTIONS/
 * underpromotions (checkpoint 4; the 2-edit application) in state.c.
 * EP, castling and promotion application is STRUCTURAL, not legal: each
 * requires the correct pieces, geometry (and, for castling, the right) but none
 * rejects own-king safety -- a pinned captor's EP, a check-exposing castle
 * (FIDE 3.8.2) or a pinned promotion is still applied and reversed here; that
 * king-safety check belongs to movegen in T005. Null moves are the final T004
 * checkpoint; they are still REJECTED cleanly here (reserved in the delta's
 * capacity).
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
 *
 * Repetition history (checkpoint 5): every applied record (a real move or a null)
 * links to its predecessor via StateInfo.prev, and Position.history points at the
 * latest applied record. The history is a caller-owned chain of records at stable
 * addresses; it is NOT capped at MAX_PLY (pre-root game records and the bounded
 * search undo array may live in separate storage). A successful setup (FEN /
 * start position / reset) starts a fresh history (Position.history == NULL); a
 * failed FEN load and pos_rebuild preserve it. pos_repetition_count (below) is
 * the production occurrence query used to exclude synthetic nulls from the
 * repetition count.
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

/* Forward-declare the undo-record tag so `Position` (position.h) may hold a
 * non-owning pointer to the latest applied record without a full include cycle.
 * The complete definition follows below; `struct StateInfo *` and the `StateInfo`
 * typedef below name the same type. */
struct StateInfo;

/* One board-square change: the square and its piece before and after the move.
 * NO_PIECE in a field means that square was / is empty. */
typedef struct SquareEdit {
  Square  sq;     /* the square that changes (0..63) */
  Piece   before; /* piece occupying sq before the move (NO_PIECE if empty) */
  Piece   after;  /* piece occupying sq after  the move (NO_PIECE if empty) */
} SquareEdit;

/* Bounded capacity. 2 edits cover every move supported by checkpoint 1
 * (origin loses the piece, destination gains it). 3 covers an en-passant
 * capture (checkpoint 2: captor origin / target landing / captured pawn) and
 * 4 covers orthodox castling (checkpoint 3: king origin/dest + rook origin/
 * dest). The apply/unmake paths loop over the populated edits, so 2, 3 and 4
 * edits are handled alike. */
enum { MAX_EDIT = 4 };

/* The reversible board change of one move: the applied Move plus the populated
 * square edits. The edits are sufficient for a future NNUE consumer to rebuild
 * the board difference without a second authoritative board. */
typedef struct MoveDelta {
  Move       move;         /* the applied move (from/to/flags/promo) */
  int        edit_count;   /* number of populated entries in `edits` (<= MAX_EDIT) */
  SquareEdit edits[MAX_EDIT];
} MoveDelta;

/* The undo record for one applied move (or null). `delta` reverses the board
 * (zero edits for a null); the prev_* scalars restore the irreversible metadata
 * exactly. Full-width u16 counters (no narrowing; the full rule-50 / fullmove
 * range is retained). `prev` links the record to the earlier record in the
 * position's repetition history (NULL for the first record after a setup); see
 * `Position.history` in position.h. A record's null-ness is identified by its
 * `delta.move == 0` / zero edits, so no extra flag is stored. */
typedef struct StateInfo {
  MoveDelta  delta;        /* reversible piece changes (0 edits for a null) */
  u64        prev_key;     /* position key before the move */
  u8         prev_side;    /* side to move before */
  u8         prev_cr;      /* castling rights before */
  Square     prev_ep;      /* exact RECORDED ep square before, or NO_SQUARE */
  u16        prev_halfmove;/* rule-50 clock before (full width) */
  u16        prev_fullmove;/* fullmove number before (full width) */
  struct StateInfo *prev;  /* earlier record in the history chain, or NULL */
} StateInfo;

/* Pseudo-legal move application on an initialized, consistent Position.
 *
 * Applies `m` to *pos and records the undo state in *state. Returns 1 on
 * success. On any failure (malformed encoding, a move kind these checkpoints
 * do not support, invalid piece geometry/path, wrong source ownership, an
 * illegal destination, a malformed en-passant record, or a counter that would
 * overflow) it returns 0 and leaves BOTH *pos and *state byte-for-byte
 * unchanged: every input is validated before any field is written.
 *
 * Supported here: quiet moves, ordinary captures, pawn single/double pushes,
 * ordinary pawn captures, en-passant captures (checkpoint 2), orthodox castling
 * (checkpoint 3) and promotions/underpromotions (checkpoint 4). EP, castling
 * and promotion application is STRUCTURAL, not legal: each validates pieces,
 * geometry (and, for castling, the right) and the recorded metadata but NOT
 * own-king safety -- a pinned captor's, a check-exposing castle (FIDE 3.8.2)
 * or a pinned promotion that is structurally valid is still applied and
 * reversed; T005 filters it (and for castling it must check the king's origin,
 * transit AND destination squares, which a final-position make/check/unmake
 * alone would miss). A Move whose from/to are valid 6-bit squares is accepted
 * by shape; semantic pseudo-legality (own-king safety, check) is NOT a
 * precondition -- that is movegen's job in T005. A successful application is
 * therefore not a legality certificate. The caller must supply a valid,
 * consistent Position (pos_validate) and a distinct StateInfo. */
int pos_make_move(struct Position *pos, StateInfo *state, Move m);

/* Reverse the move (or null) recorded in *state, restoring *pos's entire
 * logical state (mailbox, byPiece, byColor, occupancy, king squares, side,
 * castling rights, the recorded ep, both counters and the key) exactly to what
 * it was before the matching make. It also restores *pos's history head
 * (Position.history) to *state->prev, so the repetition history unwounds in the
 * same LIFO order as the board. *state is not modified and may be reused. Must
 * be called in LIFO order for a valid, consistent *pos. A null's zero-edit delta
 * reverts nothing on the board; only the scalars, the key and the history head
 * are restored. */
void pos_unmake_move(struct Position *pos, StateInfo *state);

/* Apply a null move (a pass) to *pos, recording the undo state in *state. This
 * is a distinct, explicit entry point: the ordinary pos_make_move(...) rejects
 * the move value 0 (from=to=0, no flags), which is the no-move sentinel, so a
 * pass can never be spelled accidentally. Returns 1 on success; returns 0 and
 * leaves BOTH the Position and the record byte-for-byte unchanged if either
 * pointer is NULL or the side to move is in check (a pass cannot address a
 * check). Requires an initialized, consistent Position; it does not run the full
 * malformed-board validator (that is a setup concern, not a per-move one).
 *
 * The transition: zero board edits (pieces, byPiece, byColor, occupancy, king
 * squares and castling rights are all untouched); flip the side to move; clear
 * the recorded en-passant target. QwenChess counter policy: a synthetic null
 * preserves BOTH the halfmove and the fullmove counter for either color (it
 * neither advances nor resets them), so a full-width 65535 counter is valid and
 * simply round-trips. The key is updated incrementally (toggle the side, remove
 * the old canonical EP contribution; there is no new EP) to stay exact. The
 * record links into Position.history (see pos_repetition_count). Null-move
 * pruning conditions (depth, material, zugzwang guards, consecutive-null
 * suppression) are a search concern, not applied here. */
int pos_make_null_move(struct Position *pos, StateInfo *state);

/* Occurrence count for the repetition rules: the number of times the current
 * canonical position key (Position.key) appears in the eligible history, plus
 * the current position itself if it is a real (non-null) node. A fresh setup
 * (a FEN/start position, history NULL) therefore returns 1. A position produced
 * directly by a null returns 0: a null node is never a played occurrence, and the
 * walk never crosses a null transition into earlier history (real descendants
 * after the latest null still match other real descendants within that segment).
 * Each record stores its parent's key, so before counting a record's saved key
 * the transition that PRODUCED that parent is inspected; if that transition is a
 * null, the parent is a null node (not counted) and the walk stops. This is a
 * read-only O(history) walk; it allocates nothing and touches no mutable board.
 * Undo restores the previously visible count because unmake restores
 * Position.history. This is an occurrence query only -- it is NOT
 * threefold/fivefold/rule-50 draw adjudication (that is a search policy). */
int pos_repetition_count(const struct Position *pos);

#endif /* QWC_POSITION_STATE_H */
