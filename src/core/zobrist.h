/* QwenChess core: deterministic Zobrist hashing foundations.
 *
 * Independent of the Position type. Uses a documented fixed-seed SplitMix64 so
 * the keys are identical on every run (no wall-clock or libc rand()). QwenChess
 * keys do NOT need to match Stockfish's.
 *
 * XOR convention for a position key:
 *   XOR over every occupied square of zobrist_piece(piece, square)
 *   XOR zobrist_side(side)            (applied only for black; white == 0)
 *   XOR zobrist_castling(rights)      (rights 0..15; empty state is a real key)
 *   XOR (ep_file >= 0 ? zobrist_ep_file(ep_file) : 0)
 * The en-passant file key is applied only when the future Position layer
 * establishes canonical legal ep availability. The rule-50 counter and the
 * repetition history are deliberately NOT part of this key (separate state).
 */
#ifndef QWC_CORE_ZOBRIST_H
#define QWC_CORE_ZOBRIST_H

#include "core/types.h"

/* Idempotent. Call once at startup; query helpers self-initialize if needed. */
void zobrist_init(void);

u64 zobrist_piece(Piece p, Square sq);   /* p in 1..15, sq in 0..63 */
u64 zobrist_side(Color c);               /* WHITE -> 0, BLACK -> key */
u64 zobrist_castling(u8 rights);         /* rights in 0..15 */
u64 zobrist_ep_file(int file);           /* file in 0..7 */

/* Full key from a chess-agnostic snapshot. mailbox[s] = Piece (0 = empty).
 * Out-of-table piece codes are ignored for memory safety; this does not validate
 * a board. The Position validator rejects all undefined chess piece codes.
 * ep_file = file of a canonically-legal ep target, or -1 for none. */
u64 zobrist_compute(const u8 mailbox[64], Color side, u8 rights, int ep_file);

#endif /* QWC_CORE_ZOBRIST_H */
