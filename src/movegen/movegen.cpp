/* QwenChess: legal move generation (T005 cp1 pseudo-legal; cp2 non-castling legal;
 * cp3 full legal incl. orthodox castling).
 *
 * See movegen.hpp for the contracts. The pseudo-legal generator (cp1) emits every
 * non-castling pseudo-legal candidate: ordinary piece moves/captures, pawn single/
 * double pushes and captures, all four promotions (and capture-promotions), and
 * structural en-passant captures. It is read-only: no heap, no Position copy, no
 * make/unmake, no rebuild, no king-safety query.
 *
 * The non-castling legal generator (cp2) filters those candidates for own-king
 * safety: it keeps only the moves that leave the original mover's own king
 * unattacked. It probes ONE scratch copy of the Position per call (a bounded
 * stack-local, not a second authoritative board) and one local undo record,
 * applying each candidate, asking pos_is_in_check about the mover, and reversing
 * it. The working buffer + scratch + record are stack-allocated; there is no per-
 * move heap allocation.
 *
 * The full legal generator (cp3) returns the non-castling legal moves plus the legal
 * castles. A castle is legal iff the side holds the right, the home king/rook are in
 * place with an empty path, and the king's origin, transit and destination squares
 * are all unattacked. The origin is a read-only pos_is_in_check; the transit (an
 * ordinary one-square king step) and the destination (the actual castle) are probed
 * on one normalized scratch via existing make/unmake. All probes are bounded
 * stack-locals; the caller's Position, its history and every live record are left
 * untouched on every outcome. This is a correctness baseline on the current attack
 * query; it is optimized only later, against perft/differential evidence.
 */
#include "movegen/movegen.hpp"

#include <cstddef>
#include <span>
#include <array>

#include "core/types.hpp"
#include "core/square.hpp"
#include "core/bitboard.hpp"
#include "core/attacks.hpp"

namespace qwc {

/* Append one move to `out` if there is room. Returns 0 on overflow (the caller
 * treats that as a hard stop) and never writes outside `capacity` (so it is safe
 * even when `out` is null and `capacity` is zero). */
static int mg_append(Move *out, std::size_t capacity, std::size_t *n, Move m) {
  if (*n >= capacity)
    return 0;
  out[*n] = m;
  (*n)++;
  return 1;
}

/* Destination mask for a non-pawn piece on `from`: its attack squares (sliders
 * use the combined occupancy) EXCLUDING friendly occupancy and the opposing
 * king. Own-king safety is NOT applied at this layer. */
static u64 mg_dest_mask(const Position *pos, Square from, PieceType type,
                        u64 friendly, u64 enemy_king_bb) {
  u64 att;
  switch (type) {
    case PieceType::KNIGHT: att = knight_attacks(from); break;
    case PieceType::BISHOP: att = bishop_attacks(from, pos->occ); break;
    case PieceType::ROOK:   att = rook_attacks(from, pos->occ); break;
    case PieceType::QUEEN:  att = queen_attacks(from, pos->occ); break;
    case PieceType::KING:   att = king_attacks(from); break;
    default:     att = 0; break;   /* PieceType::PAWN / PieceType::NO_PIECE_TYPE: handled by the pawn path */
  }
  return att & ~friendly & ~enemy_king_bb;
}

/* Emit the pseudo-legal moves of one friendly pawn on `from`. Covers the single
 * and double push, the two diagonal captures, and (on the final rank) all four
 * promotions for each available push or capture. Returns 0 on capacity overflow
 * (hard stop), 1 otherwise. `dir` is +8 for white / -8 for black; `start_r` and
 * `promo_r` are the (0-indexed) start and promotion ranks for the side. */
static int mg_gen_pawn(const Position *pos, Square from, Move *out, std::size_t capacity,
                       std::size_t *n, int dir, int start_r, int promo_r,
                       u64 enemy, u64 enemy_king_bb) {
  const int f  = file_of(from);
  const int r  = rank_of(from);
  const int tr = r + (dir > 0 ? 1 : -1);   /* the forward target rank index */

  /* Push (same file). Guards against file wrap (none) and rank/edge overflow. */
  if (tr >= 0 && tr < RANK_NB) {
    const Square push = square_of(f, tr);
    if ((pos->occ & square_bb(push)) == 0) {   /* target empty */
      if (tr == promo_r) {
        for (u32 pt = to_under(PieceType::KNIGHT); pt <= to_under(PieceType::QUEEN); pt++)
          if (!mg_append(out, capacity, n, make_move(from, push, (u32)MoveFlag::MV_PROMO, pt)))
            return 0;
      } else {
        if (!mg_append(out, capacity, n, make_move(from, push, 0u, 0u)))
          return 0;
      }
      /* Double push: only from the start rank, both squares empty. (Never to the
       * promotion rank, so a start-rank pawn always has a valid 3rd/6th rank.) */
      if (r == start_r) {
        const Square dbl = square_of(f, tr + (dir > 0 ? 1 : -1));
        if ((pos->occ & square_bb(dbl)) == 0)
          if (!mg_append(out, capacity, n, make_move(from, dbl, 0u, 0u)))
            return 0;
      }
    }
  }

  /* Captures: the two diagonal forward squares; require an enemy non-king piece. */
  for (int sfx = -1; sfx <= 1; sfx += 2) {
    const int cf = f + sfx;
    if (cf < 0 || cf >= FILE_NB)
      continue;                              /* no file wrap */
    if (tr < 0 || tr >= RANK_NB)
      continue;                              /* pawn on the last rank: no forward square */
    const Square to = square_of(cf, tr);
    const u64 to_bb = square_bb(to);
    if ((enemy & to_bb) == 0)
      continue;                              /* needs an enemy piece on the target */
    if (to_bb == enemy_king_bb)
      continue;                              /* never capture the king */
    if (tr == promo_r) {
      for (u32 pt = to_under(PieceType::KNIGHT); pt <= to_under(PieceType::QUEEN); pt++)
        if (!mg_append(out, capacity, n, make_move(from, to, (u32)MoveFlag::MV_PROMO, pt)))
          return 0;
    } else {
      if (!mg_append(out, capacity, n, make_move(from, to, 0u, 0u)))
        return 0;
    }
  }
  return 1;
}

/* ---- primary: bounded (span) generation ---- */

bool movegen_pseudo_noncastling(const Position& pos, std::span<Move> output,
                                std::size_t& count) {
  count = 0;   /* zero on entry; written to the full count only on success */
  std::size_t n = 0;
  Move *out = output.data();        /* may be null when output.size() is 0 */
  std::size_t cap = output.size();

  const Color   side    = pos.side;
  const u64     friendly = pos.by_color[to_under(side)];
  const u64     enemy    = pos.by_color[1 - to_under(side)];
  const u64     enemy_king_bb =
      square_bb((side == Color::WHITE) ? pos.black_king_sq : pos.white_king_sq);
  const int  dir     = (side == Color::WHITE) ? 8  : -8;
  const int  start_r = (side == Color::WHITE) ? 1  : 6;   /* 2nd / 7th rank */
  const int  promo_r = (side == Color::WHITE) ? 7  : 0;   /* 8th / 1st rank */
  const u64  side_pawns = pos.by_piece[(side == Color::WHITE) ? to_under(Piece::W_PAWN) : to_under(Piece::B_PAWN)];

  /* Ordinary pieces (knight/bishop/rook/queen/king) + pawns, in a fixed,
   * deterministic square order. */
  for (u64 bits = friendly; bits; bits = pop_lsb(bits)) {
    const Square    from = (Square)lsb_index(bits);
    const PieceType type = piece_type(pos.mailbox[from]);
    if (type == PieceType::PAWN) {
      if (!mg_gen_pawn(&pos, from, out, cap, &n,
                       dir, start_r, promo_r, enemy, enemy_king_bb))
        return false;   /* overflow: `count` stays 0 */
    } else {
      const u64 dest = mg_dest_mask(&pos, from, type, friendly, enemy_king_bb);
      for (u64 d = dest; d; d = pop_lsb(d))
        if (!mg_append(out, cap, &n,
                       make_move(from, (Square)lsb_index(d), 0u, 0u)))
          return false;   /* overflow */
    }
  }

  /* En passant: structural, not legal. The recorded target plus its metadata
   * (the just-pushed enemy pawn, an empty landing, an adjacent friendly captor)
   * are checked inline; king safety is deliberately NOT (pos_ep_is_legal /
   * pos_canon_ep_file / pos_ep_capture_is_safe are not consulted). Up to two
   * captors yield two distinct moves; a pinned captor is still a candidate. */
  if (pos.ep_sq != NO_SQUARE) {
    const int ef = file_of(pos.ep_sq);
    const int er = rank_of(pos.ep_sq);
    const int captor_r = er + (side == Color::WHITE ? -1 : 1);   /* one rank behind */
    if (captor_r >= 0 && captor_r < RANK_NB) {
      const u64  cap_bb   = square_bb(square_of(ef, captor_r));  /* the taken pawn */
      const u64  landing  = square_bb(pos.ep_sq);
      const int  enemy_pawn = (side == Color::WHITE) ? to_under(Piece::B_PAWN) : to_under(Piece::W_PAWN);
      const int  enemy_present = (pos.by_piece[enemy_pawn] & cap_bb) != 0;
      const int  landing_empty = (pos.occ & landing) == 0;
      if (enemy_present && landing_empty) {
        for (int sfx = -1; sfx <= 1; sfx += 2) {
          const int cf = ef + sfx;
          if (cf < 0 || cf >= FILE_NB)
            continue;
          const Square captor = square_of(cf, captor_r);
          if ((side_pawns & square_bb(captor)) == 0)
            continue;                              /* needs an adjacent friendly captor */
          if (!mg_append(out, cap, &n, make_move(captor, pos.ep_sq, (u32)MoveFlag::MV_EP, 0u)))
            return false;                          /* overflow */
        }
      }
    }
  }

  count = n;   /* success: the complete move count */
  return true;
}

/* ---- adapter: legacy pointer form ----
 *
 * Validates its legacy null-pointer cases (a null `pos` or `out` fails, even with
 * zero capacity) before constructing the span, then delegates to the primary. The
 * `count` pointer remains optional. */
int movegen_pseudo_noncastling(const Position *pos, Move *out, std::size_t capacity,
                               std::size_t *count) {
  if (pos == nullptr || out == nullptr) {
    if (count)
      *count = 0;
    return 0;
  }
  std::size_t n = 0;
  bool ok = movegen_pseudo_noncastling(*pos, std::span<Move>(out, capacity), n);
  if (count)
    *count = n;   /* the primary already normalized n to 0 on failure */
  return ok ? 1 : 0;
}

/* ---- legal non-castling (T005 cp2) ---------------------------------------- */

/* Upper bound on the non-castling pseudo-legal candidate count, sized for the fixed
 * local candidate buffer: at most 16 friendly pieces, each with at most 27
 * destinations (a queen's maximum; a pawn's promotion-expanded 12 fits under it),
 * plus up to two separately-appended en-passant captures. 16*27 + 2 == 434. This is
 * a conservative capacity for the buffer, NOT a claim about the maximum legal count. */
inline constexpr std::size_t MLEGAL_MAX_CANDIDATES = 16 * 27 + 2;   /* 434 */

bool movegen_legal_noncastling(const Position& pos, std::span<Move> output,
                               std::size_t& count) {
  count = 0;   /* zero on entry; written to the full legal count only on success */
  Move *out = output.data();        /* may be null when output.size() is 0 */
  std::size_t out_cap = output.size();
  std::size_t out_n = 0;

  /* 1. Collect the pseudo-legal candidates into a fixed local buffer, independent of
   *    the caller's output capacity. The bound above is a proven upper bound, so the
   *    generator cannot overflow it; a failure here would be a real integration bug. */
  std::array<Move, MLEGAL_MAX_CANDIDATES> cand;
  std::size_t ncand = 0;
  if (!movegen_pseudo_noncastling(pos, std::span<Move>(cand.data(), MLEGAL_MAX_CANDIDATES), ncand))
    return false;   /* count stays 0 */

  /* 2. ONE scratch copy per call (a temporary copy of the existing representation,
   *    not a second authoritative board and not an undo-stack redesign). Normalize
   *    the counters to 0/1 and detach the history: this king-safety probe makes and
   *    unmakes one move at a time from a clean counter state, so it must not reject a
   *    move merely because the caller's halfmove/fullmove counters sit at the u16
   *    boundary (65535). Counters and the history are not part of the position key
   *    (and a valid recorded EP already requires a zero halfmove), so the board, side,
   *    rights, raw EP and derived caches remain consistent -- no rebuild is needed
   *    for this normalization. The caller's counters/history are left untouched. */
  Position scratch = pos;
  scratch.halfmove = 0;
  scratch.fullmove = 1;
  scratch.history = nullptr;   /* detach: the probe does not touch the caller's chain */

  const Color   mover  = pos.side;   /* the ORIGINAL mover, whose own king we test */
  StateInfo     state{};            /* one local undo record, reused after each unmake */

  /* 3. Filter each candidate: apply it to the scratch, test the original mover's king
   *    on the resulting board, then reverse. The geometric attack query (pinned enemy
   *    pieces and the enemy king included) is the single source of truth; a giving
   *    check is legal, so the side to move after the move is never tested. The same
   *    rule holds in and out of check, so it retains quiet blocks, checker captures,
   *    legal pin-line moves, sacrifices, all four promotions and EP evasions. */
  for (std::size_t i = 0; i < ncand; i++) {
    const Move m = cand[i];
    if (!pos_make_move(&scratch, &state, m))
      return false;   /* an application failure is an integration error, not a rejection */
    const bool safe = !pos_is_in_check(&scratch, mover);
    pos_unmake_move(&scratch, &state);   /* always reverse before the next / return */
    if (!safe)
      continue;
    if (out_n >= out_cap)
      return false;   /* insufficient capacity: count stays 0; a prefix may remain */
    out[out_n++] = m;
  }

  count = out_n;   /* success: the complete legal count */
  return true;
}

/* ---- full legal (T005 cp3): non-castling legal moves + legal castles ---------- */

/* The castling geometry for one side-to-move castle at the mover's home rank `hr`:
 * the rook origin (the encoded castle's `to`), the path squares that must be empty,
 * the one-square transit king step, and the encoded castle (king origin -> rook
 * origin, MV_CASTLE, zero promotion payload). The king origin `e` (e1/e8) is shared
 * by both sides and is passed in. Square semantics follow FIDE 3.9: short castling
 * needs f/g empty; long castling needs b/c/d empty (the b-square need only be empty --
 * it is never a king square and need not be unattacked). */
struct CastleGeo {
  Square king_from;   /* e1 / e8 (the shared origin) */
  Square rook_from;   /* h1/h8 (kingside) or a1/a8 (queenside): the castle's `to` */
  u64    path_bb;     /* the path squares that must be empty (2 kingside / 3 queenside) */
  Square transit;     /* the one-square king step toward the rook (f/d or f8/d8) */
  Move   castle;      /* make_move(king_from, rook_from, MV_CASTLE, 0) */
};

static void mg_castle_geo(bool kingside, int hr, Square e, CastleGeo *g) {
  const Square rook = square_of(kingside ? FILE_H : FILE_A, hr);
  const Square tran = square_of(kingside ? FILE_F : FILE_D, hr);
  g->king_from = e;
  g->rook_from = rook;
  g->transit   = tran;
  g->path_bb   = square_bb(tran);   /* f or d is always one of the path squares */
  if (kingside) {
    g->path_bb |= square_bb(square_of(FILE_G, hr));                 /* g1 / g8 */
  } else {
    g->path_bb |= square_bb(square_of(FILE_C, hr))                  /* c1 / c8 (the destination, in path) */
               |  square_bb(square_of(FILE_B, hr));                 /* b1 / b8 (empty only) */
  }
  g->castle = make_move(e, rook, (u32)MoveFlag::MV_CASTLE, 0u);
}

/* Probe one candidate castle for king safety on the caller's normalized scratch.
 * The ORIGIN is tested by the caller (a checked king cannot castle at all); this
 * covers what that does not -- the home rook and the empty path (read from the
 * authoritative `pos`) -- and the transit + destination, probed on `scratch` via the
 * proven make/unmake and always restored. Returns 1 if the castle is legal, 0 if it
 * is safely excluded (a blocked path or an attacked transit/destination), and -1 if a
 * make failed unexpectedly after the structural prechecks (an internal error the
 * caller propagates as a hard fail). `scratch` is left restored on every return. */
static int mg_probe_castle(const Position &pos, Position *scratch, Color mover,
                           const CastleGeo &g) {
  const Piece rook = (mover == Color::WHITE) ? Piece::W_ROOK : Piece::B_ROOK;

  /* Structural (read-only on the authoritative `pos`): the home rook is present and
   * the path is empty. (The right + the king on its home e-square are the caller's.) */
  if (pos.mailbox[g.rook_from] != rook) return 0;
  if ((pos.occ & g.path_bb) != 0)       return 0;

  StateInfo st{};

  /* Transit: an ordinary one-square king step onto the path, the rook still at home.
   * The king lands on `transit`; pos_is_in_check then reports whether that square is
   * attacked (geometrically -- pinned enemy sliders and the enemy king included). A
   * failed make here would contradict the structural prechecks above. */
  const Move step = make_move(g.king_from, g.transit, 0u, 0u);
  if (pos_make_move(scratch, &st, step) != 1) return -1;
  const bool transit_safe = pos_is_in_check(scratch, mover) == 0;
  pos_unmake_move(scratch, &st);
  if (!transit_safe) return 0;

  /* Destination: the actual castle (king -> g/c, rook relocated). The king lands on
   * the destination with the rook in its post-castle square, so this tests the final
   * king square on the true post-castle board (a friendly rook that now blocks an
   * attack line is accounted for). Giving a check is legal, so the opponent is never
   * tested. */
  if (pos_make_move(scratch, &st, g.castle) != 1) return -1;
  const bool dest_safe = pos_is_in_check(scratch, mover) == 0;
  pos_unmake_move(scratch, &st);
  if (!dest_safe) return 0;

  return 1;   /* origin (caller) + transit + destination all safe */
}

bool movegen_legal(const Position& pos, std::span<Move> output, std::size_t& count) {
  count = 0;   /* zero on entry; the complete legal count is published only on success */
  Move *out = output.data();              /* may be null when output.size() is 0 */
  const std::size_t out_cap = output.size();

  /* 1. The non-castling legal moves, into a bounded local buffer independent of the
   *    caller's capacity. MLEGAL_MAX_CANDIDATES bounds the pseudo count, hence the
   *    legal count, so this cannot overflow for a valid position. It bounds only the
   *    non-castling part; the combined output is the caller's separate span, which is
   *    capacity-checked for the full total (these moves plus up to two castles). */
  std::array<Move, MLEGAL_MAX_CANDIDATES> ncbuf;
  std::size_t nc = 0;
  if (!movegen_legal_noncastling(pos, std::span<Move>(ncbuf.data(), MLEGAL_MAX_CANDIDATES), nc))
    return false;   /* count stays 0 */

  /* 2. Publish the non-castling moves into the caller's buffer (bounds-checked). The
   *    complete set is these plus up to two castles, so a buffer that fits only the
   *    non-castling moves still fails below (count 0), not a silent truncation. */
  if (nc > out_cap)
    return false;   /* the non-castling part alone does not fit */
  for (std::size_t i = 0; i < nc; i++)
    out[i] = ncbuf[i];
  std::size_t out_n = nc;

  /* 3. Legal castles. Only the side to move can castle, and only from its home king
   *    square; a checked king cannot castle (its evasions are already in `nc`). The
   *    origin is a read-only query on `pos`; the transit and destination are probed
   *    on one normalized scratch (counters 0/1, history detached -- none of which is
   *    part of the position key, and a valid recorded EP already implies halfmove 0).
   *    The caller's Position is never normalized. */
  const bool   white = (pos.side == Color::WHITE);
  const int    hr    = white ? RANK_1 : RANK_8;
  const Square e     = square_of(FILE_E, hr);
  const Square ksq   = white ? pos.white_king_sq : pos.black_king_sq;
  if (ksq == e && pos_is_in_check(&pos, pos.side) == 0) {
    Position scratch = pos;
    scratch.halfmove = 0;
    scratch.fullmove = 1;
    scratch.history  = nullptr;   /* detach: the probes make/unmake from a clean state */

    const u8 k_right = white ? CR_WK : CR_BK;
    const u8 q_right = white ? CR_WQ : CR_BQ;
    for (int sfx = 0; sfx < 2; sfx++) {   /* kingside then queenside (deterministic) */
      const bool kingside = (sfx == 0);
      const u8   right    = kingside ? k_right : q_right;
      if ((pos.cr & right) == 0)
        continue;                          /* no right on this side -> no castle */
      CastleGeo g;
      mg_castle_geo(kingside, hr, e, &g);
      const int r = mg_probe_castle(pos, &scratch, pos.side, g);
      if (r < 0)
        return false;   /* an unexpected make failure is an API failure (count 0) */
      if (r != 1)
        continue;       /* safely excluded (blocked path or an attacked transit/dest) */
      if (out_n >= out_cap)
        return false;   /* insufficient capacity for the full set -> count 0 */
      out[out_n++] = g.castle;
    }
  }

  count = out_n;   /* success: the complete orthodox legal count (non-castling + castles) */
  return true;
}

}  // namespace qwc
