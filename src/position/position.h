/* QwenChess: Position -- the one authoritative chess state.
 *
 * Contract (docs/ARCHITECTURE.md): Position is the single board. Fields split
 * into caller-PROVIDED inputs and DERIVED caches:
 *   inputs  : mailbox[64], side, cr, ep_sq, halfmove, fullmove
 *   derived : byPiece[16], byColor[2], occ, wKingSq, bKingSq, key   (rebuild)
 * Piece codes, square numbering and sentinels are unchanged (see core/types.h).
 *
 * Ownership/lifetime: a Position is a plain, stack- or worker-allocated value;
 * nothing here points at another Position or a StateInfo. The undo record and
 * the full repetition history are added in T004. Position/StateInfo contain NO
 * NNUE types, feature indices or evaluator caches. No heap allocation is
 * performed by the functions in this checkpoint.
 *
 * En passant: `ep_sq` is the RECORDED target -- the square a double push would
 * let an adjacent pawn capture onto -- retained even when no capture is actually
 * legal, matching Stockfish (the ep square is recorded after any double push).
 * It must be structurally consistent with a just-played double push (see
 * pos_ep_is_valid_meta: correct rank for the side, an empty destination, the
 * enemy pawn on the just-pushed square, a vacant double-push origin, and a zero
 * halfmove clock). A structurally valid but uncapturable target is a *valid*
 * Position. pos_ep_is_legal() decides whether a capture is actually king-safe;
 * pos_canon_ep_file() is the file the KEY uses and is -1 whenever no capture is
 * legal, so an uncapturable ep contributes nothing to the key or to repetition
 * identity. The FEN loader (fen.c) sets `ep_sq` to the recorded target and
 * rejects malformed ones; pos_reset / pos_set_startpos carry no ep.
 */
#ifndef QWC_POSITION_POSITION_H
#define QWC_POSITION_POSITION_H

#include "core/types.h"
#include "core/bitboard.h"

/* Documented bound: halfmove clock beyond this is a 50-move draw. The counter
 * is stored wide (u16); values above the bound are structurally rejected, and
 * no counter ever silently wraps (the u16 domain is validated at the boundary). */
#define POS_HM_MAX 100

typedef struct Position {
  /* -- inputs (set by a setup path; T004 make/unmake) -- */
  u8            mailbox[SQ_NB];  /* Piece (0 = empty); the authoritative board */
  Color         side;            /* side to move: WHITE / BLACK */
  u8            cr;              /* castling rights: CR_* (0..15) */
  Square        ep_sq;           /* recorded ep target square, or NO_SQUARE */
  HalfMoveClock halfmove;        /* rule-50 clock (validated [0,POS_HM_MAX]) */
  FullMoveNumber fullmove;       /* FEN fullmove number, 1-based (>= 1) */
  /* -- derived caches (recomputed by pos_rebuild; never trusted as inputs) -- */
  u64           byPiece[PIECE_NB]; /* one bitboard per piece (1..15) */
  u64           byColor[COLOR_NB]; /* WHITE / BLACK occupancy */
  u64           occ;             /* byColor[WHITE] | byColor[BLACK] */
  Square        wKingSq, bKingSq;  /* king squares, or NO_SQUARE */
  u64           key;             /* zobrist position key (canonical ep) */
} Position;

/* Cold setup / rebuild. */
void pos_reset(Position *pos);          /* empty board, side WHITE, all rights 0 */
void pos_set_startpos(Position *pos);   /* orthodox start: W, CR_ALL, no ep, hm 0, fm 1 */

/* Recompute every derived field (bitboards, occupancy, king squares, key) from
 * the inputs. Cold path only (not the make/unmake hot path, which lands in
 * T004); safe to call on any Position. Does not touch the inputs. */
void pos_rebuild(Position *pos);

/* Position key for a snapshot (the exact zobrist convention, canonical ep). */
u64 pos_key(const Position *pos);

/* Canonical ep file: file of `ep_sq` if a fully-legal en-passant capture exists
 * (see pos_ep_is_legal), else -1. This is the ep component of the position key,
 * so an un-capturable ep square never affects the key or repetition identity. */
int pos_canon_ep_file(const Position *pos);

/* True iff a fully-legal en-passant capture exists for the recorded ep square:
 * the double-pushed enemy pawn and an adjacent friendly captor (there can be up
 * to two, on files f-1 and f+1) exist, and at least one capture leaves the
 * capturing king not in check. Depends only on the mailbox + side (it locates the
 * king there); no legal move generation is performed. */
int pos_ep_is_legal(const Position *pos);

/* True iff the recorded ep target is acceptable: absent, or structurally
 * consistent with a double push just played (correct rank for the side, an empty
 * destination, the enemy pawn on the just-pushed square, a vacant double-push
 * origin, and a zero halfmove clock). This metadata invariant is upheld by every
 * Position and enforced by pos_validate; legality is separate (pos_ep_is_legal). */
int pos_ep_is_valid_meta(const Position *pos);

/* Read-only structural validator. Returns 1 if the Position is internally
 * consistent (mailbox valid; derived caches == a from-scratch recompute; exactly
 * one king per color; metadata in range; key == recomputed). Returns 0 and sets
 * *why to a short reason (may be NULL) otherwise. Never mutates *pos. This is a
 * representation-consistency check, NOT a move-legality or reachability proof. */
int pos_validate(const Position *pos, const char **why);

#endif /* QWC_POSITION_POSITION_H */
