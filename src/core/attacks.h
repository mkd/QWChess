/* QwenChess core: portable attack generation.
 *
 * Production uses precomputed directional rays trimmed at the nearest blocker
 * (a slow, obviously-correct implementation whose interface is replaceable by a
 * measured optimization later -- magics/PEXT/SIMD are deferred to M6).
 *
 * Call attacks_init() once at startup; query functions also self-initialize if
 * not already done. Tables are immutable after init.
 *
 * Contract (every mask EXCLUDES the originating square):
 *  - sliders (rook/bishop/queen) take COMBINED occupancy; they include the
 *    first blocker on each ray and exclude squares beyond it. They do NOT
 *    filter friendly pieces or test king safety.
 *  - a square's own occupancy bit does not affect the returned set.
 *  - pawn_attacks returns capture squares only (not the forward push).
 *  - for NO_SQUARE / out-of-range squares every function returns 0.
 */
#ifndef QWC_CORE_ATTACKS_H
#define QWC_CORE_ATTACKS_H

#include "core/types.h"
#include "core/bitboard.h"

void attacks_init(void);

u64 pawn_attacks(Color c, Square sq);   /* capture masks */
u64 knight_attacks(Square sq);
u64 king_attacks(Square sq);
u64 rook_attacks(Square sq, u64 occ);
u64 bishop_attacks(Square sq, u64 occ);
u64 queen_attacks(Square sq, u64 occ);

#endif /* QWC_CORE_ATTACKS_H */
