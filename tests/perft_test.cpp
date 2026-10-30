/* QwenChess T005 cp4 test: perft regression + traversal semantics.
 *
 * Verifies the shared perft() diagnostic (src/diagnostics/perft.cpp) against the
 * independently-verified standard perft fixtures (chess 1.11.2; see the perft
 * appendix in docs/PLAN.md) and the traversal's required semantics. The normal
 * suite (`make test`) runs the BOUNDED depths (fast); `make test-perft` (or this
 * binary with `--full`) runs every supplied depth, including the deepest.
 *
 * Fixtures are the standard orthodox regression positions; every count was
 * independently recomputed with the pinned `chess` 1.11.2. This test is a
 * perft/diagnostic regression, not a UCI/search implementation (it evaluates
 * nothing and chooses no move).
 *
 * Groups: (1) perft counts at the bounded or full depths; (2) depth-zero and
 * terminal (mate/stalemate) semantics; (3) invalid depths; (4) divide sums;
 * (5) determinism; (6) preservation of a live pre-root history chain; and
 * (7) independence from high counters and repetition history. Comparisons use the
 * full logical fields (including the derived caches, key and history head); struct
 * padding is not logical state.
 */
#include "diagnostics/perft.hpp"
#include "movegen/movegen.hpp"
#include "position/position.hpp"
#include "position/state.hpp"
#include "position/fen.hpp"
#include "core/types.hpp"
#include "core/square.hpp"
#include "test_util.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

using namespace qwc;

static Square sq(int f, int r) { return square_of((Square)f, (Square)r); }
static Move  mv(int f, int r, int tf, int tr) { return make_move(sq(f, r), sq(tf, tr), 0u, 0u); }

/* ---- appendix fixtures (verified with chess 1.11.2) ------------------------- */

struct Fixture {
  const char *name;
  const char *fen;
  const u64 *counts;   /* counts[d] is the perft count at depth d; counts[0] == 1 */
  int n;               /* number of depths (count array length) */
};

static const u64 kStartpos[]  = {1, 20, 400, 8902, 197281, 4865609};
static const u64 kKiwipete[]  = {1, 48, 2039, 97862, 4085603};
static const u64 kRookPawn[]  = {1, 14, 191, 2812, 43238, 674624};
static const u64 kPromCastl[] = {1, 6, 264, 9467, 422333};
static const u64 kPromTact[]  = {1, 44, 1486, 62379, 2103487};

static const Fixture kFixtures[] = {
  {"startpos",           "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",     kStartpos,  6},
  {"kiwipete",           "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", kKiwipete, 5},
  {"rook_pawn_endgame",  "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",      kRookPawn,  6},
  {"promotion_castling", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", kPromCastl, 5},
  {"promotion_tactics",  "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",  kPromTact,  5},
};

/* Inclusive maximum depth for the ordinary (bounded) suite, per fixture. The
 * `--full` / `make test-perft` gate runs the complete count array instead. */
static const int kBoundedMaxDepth[] = {3, 3, 4, 3, 3};

/* ---- helpers ---------------------------------------------------------------- */

static bool load(const char *fen, Position &pos) {
  std::string_view err;
  return fen_load(pos, std::string_view(fen), &err);
}

/* Full logical Position equality (board, scalars, derived caches, key, and the
 * history-head pointer). Struct padding is not compared. */
static int pos_logical_equal(const Position *a, const Position *b) {
  for (int s = 0; s < SQ_NB; s++) if (a->mailbox[s] != b->mailbox[s]) return 0;
  if (a->side != b->side) return 0;
  if (a->cr != b->cr) return 0;
  if (a->ep_sq != b->ep_sq) return 0;
  if (a->halfmove != b->halfmove) return 0;
  if (a->fullmove != b->fullmove) return 0;
  for (int p = 0; p < PIECE_NB; p++) if (a->by_piece[p] != b->by_piece[p]) return 0;
  for (int c = 0; c < COLOR_NB; c++) if (a->by_color[c] != b->by_color[c]) return 0;
  if (a->occ != b->occ) return 0;
  if (a->white_king_sq != b->white_king_sq) return 0;
  if (a->black_king_sq != b->black_king_sq) return 0;
  if (a->key != b->key) return 0;
  if (a->history != b->history) return 0;
  return 1;
}

/* Full logical StateInfo equality (the delta + every restored scalar + the prev
 * link). Struct padding is not compared. */
static int stateinfo_equal(const StateInfo *a, const StateInfo *b) {
  if (a->delta.move != b->delta.move) return 0;
  if (a->delta.edit_count != b->delta.edit_count) return 0;
  for (int i = 0; i < a->delta.edit_count; i++) {
    if (a->delta.edits[i].sq != b->delta.edits[i].sq) return 0;
    if (a->delta.edits[i].before != b->delta.edits[i].before) return 0;
    if (a->delta.edits[i].after != b->delta.edits[i].after) return 0;
  }
  if (a->prev_key != b->prev_key) return 0;
  if (a->prev_side != b->prev_side) return 0;
  if (a->prev_cr != b->prev_cr) return 0;
  if (a->prev_ep != b->prev_ep) return 0;
  if (a->prev_halfmove != b->prev_halfmove) return 0;
  if (a->prev_fullmove != b->prev_fullmove) return 0;
  if (a->prev != b->prev) return 0;
  return 1;
}

/* Reproduce the divide: the sum of perft(depth-1) over each root move. The input
 * `pos` is copied (and its history detached) so the traversal's make/unmake do not
 * touch the caller's records. Returns false on any generation/make failure. */
static bool divide_sum(const Position &pos, int depth, u64 &sum) {
  sum = 0;
  std::array<Move, kPerftMoveCapacity> moves{};
  std::size_t count = 0;
  if (!movegen_legal(pos, moves, count)) return false;
  Position cur = pos;
  cur.history = nullptr;   // detach: the divide's makes use only local undo records
  for (std::size_t i = 0; i < count; ++i) {
    StateInfo rec{};
    if (!pos_make_move(&cur, &rec, moves[i])) return false;
    u64 sub = 0;
    const bool ok = perft(cur, depth - 1, sub);
    pos_unmake_move(&cur, &rec);
    if (!ok) return false;
    if (sub > (std::numeric_limits<u64>::max() - sum)) return false;
    sum += sub;
  }
  return true;
}

/* Apply the startpos and 1.e4 e5, leaving a board with a live 2-record history. */
static bool build_with_history(Position &pos, StateInfo *hist, int &hist_n) {
  if (!load("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", pos)) return false;
  hist_n = 0;
  if (!pos_make_move(&pos, &hist[hist_n++], mv(4, 1, 4, 3))) return false;  // e2e4
  if (!pos_make_move(&pos, &hist[hist_n++], mv(4, 6, 4, 4))) return false;  // e7e5
  return true;
}

/* ---- test groups ------------------------------------------------------------- */

/* (1) perft counts: bounded for the normal suite, full with --full / test-perft. */
static int t_counts(bool full) {
  qwc_begin(full ? "perft.counts.full" : "perft.counts.bounded");
  char label[64];
  for (int f = 0; f < (int)(sizeof kFixtures / sizeof kFixtures[0]); ++f) {
    Position pos;
    QWC_TRUE(load(kFixtures[f].fen, pos), "load fixture");
    const int maxd = full ? kFixtures[f].n : (kBoundedMaxDepth[f] + 1);
    for (int d = 0; d < maxd; ++d) {
      u64 nodes = 0;
      std::snprintf(label, sizeof label, "%s d%d", kFixtures[f].name, d);
      QWC_TRUE(perft(pos, d, nodes), label);
      QWC_EQ_U64(nodes, kFixtures[f].counts[d], label);
    }
  }
  return qwc_end();
}

/* (2) depth-zero + terminal (mate/stalemate) semantics. */
static int t_terminal() {
  qwc_begin("perft.terminal");
  // A simple checkmate (black to move, mated by the queen on g7).
  {
    Position pos;
    QWC_TRUE(load("7k/6Q1/5K2/8/8/8/8/8 b - - 0 1", pos), "load mate");
    u64 n0 = 99, n1 = 99;
    QWC_TRUE(perft(pos, 0, n0), "mate d0 runs");
    QWC_EQ_U64(n0, 1, "mate depth 0 == 1");
    QWC_TRUE(perft(pos, 1, n1), "mate d1 runs");
    QWC_EQ_U64(n1, 0, "mate depth 1 == 0");
  }
  // A stalemate (black to move, not in check, no legal moves).
  {
    Position pos;
    QWC_TRUE(load("k7/8/1Q6/5K2/8/8/8/8 b - - 0 1", pos), "load stalemate");
    u64 n0 = 99, n1 = 99;
    QWC_TRUE(perft(pos, 0, n0), "stale d0 runs");
    QWC_EQ_U64(n0, 1, "stalemate depth 0 == 1");
    QWC_TRUE(perft(pos, 1, n1), "stale d1 runs");
    QWC_EQ_U64(n1, 0, "stalemate depth 1 == 0");
  }
  return qwc_end();
}

/* (3) invalid depths: negative and above MAX_PLY fail with nodes 0. */
static int t_invalid_depth() {
  qwc_begin("perft.invalid_depth");
  Position pos;
  QWC_TRUE(load("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", pos), "load");
  u64 n = 99;
  QWC_TRUE(!perft(pos, -1, n), "negative depth rejected");
  QWC_EQ_U64(n, 0, "negative depth -> nodes 0");
  n = 99;
  QWC_TRUE(!perft(pos, MAX_PLY + 1, n), "depth > MAX_PLY rejected");
  QWC_EQ_U64(n, 0, "depth > MAX_PLY -> nodes 0");
  // A valid boundary depth is accepted (do not run the astronomically large 256).
  QWC_TRUE(perft(pos, 0, n), "depth 0 accepted");
  QWC_EQ_U64(n, 1, "depth 0 == 1");
  return qwc_end();
}

/* (4) divide sums: the per-root-move subtree counts sum to the ordinary perft. */
static int t_divide() {
  qwc_begin("perft.divide");
  Position pos;
  QWC_TRUE(load("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", pos), "load");
  for (int depth = 1; depth <= 3; ++depth) {
    u64 total = 0, sum = 0;
    char label[32];
    std::snprintf(label, sizeof label, "startpos d%d", depth);
    QWC_TRUE(perft(pos, depth, total), label);
    QWC_TRUE(divide_sum(pos, depth, sum), label);
    QWC_EQ_U64(sum, total, "divide sum == perft total");
  }
  return qwc_end();
}

/* (5) determinism: the same input yields the same count on every run. */
static int t_determinism() {
  qwc_begin("perft.determinism");
  Position pos;
  QWC_TRUE(load("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", pos), "load");
  u64 a = 0, b = 0, c = 0;
  QWC_TRUE(perft(pos, 3, a), "run 1");
  QWC_TRUE(perft(pos, 3, b), "run 2");
  QWC_TRUE(perft(pos, 3, c), "run 3");
  QWC_TRUE(a == b && b == c, "deterministic count");
  return qwc_end();
}

/* (6) preservation: a live pre-root history chain + the Position are untouched. */
static int t_preservation() {
  qwc_begin("perft.preservation");
  Position pos;
  std::array<StateInfo, 8> hist{};
  int hist_n = 0;
  QWC_TRUE(build_with_history(pos, hist.data(), hist_n), "build history");
  QWC_TRUE(pos.history != nullptr && hist_n == 2, "live history present");
  Position before = pos;
  StateInfo hist_before[8];
  for (int i = 0; i < 8; i++) hist_before[i] = hist[i];
  u64 nodes = 0;
  QWC_TRUE(perft(pos, 3, nodes), "perft runs");
  QWC_TRUE(pos_logical_equal(&pos, &before), "Position preserved");
  for (int i = 0; i < hist_n; i++)
    QWC_TRUE(stateinfo_equal(&hist[i], &hist_before[i]), "history record preserved");
  return qwc_end();
}

/* (7) independence: the count ignores high counters and repetition history. The
 * board is the same in all three; only the counters and the history differ. */
static int t_independence() {
  qwc_begin("perft.independence");
  // X = the board after 1.e4 e5, as a fresh FEN (counters 0/2, no history).
  Position a;
  QWC_TRUE(load("rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2", a), "load X");
  // b = the same board reached via 1.e4 e5 (live history, advanced counters).
  Position b;
  std::array<StateInfo, 8> hist{};
  int hist_n = 0;
  QWC_TRUE(build_with_history(b, hist.data(), hist_n), "build b");
  int board_match = 1;   // a and b must be the same board (the history differs)
  for (int s = 0; s < SQ_NB; s++) if (a.mailbox[s] != b.mailbox[s]) board_match = 0;
  QWC_TRUE(board_match, "a and b are the same board");
  // bp = b with high bookkeeping counters (same board, same live history).
  Position bp = b;
  bp.halfmove = 60000;
  bp.fullmove = 50000;

  u64 na = 0, nb = 0, nbp = 0;
  QWC_TRUE(perft(a, 2, na), "perft a");
  QWC_TRUE(perft(b, 2, nb), "perft b");
  QWC_TRUE(perft(bp, 2, nbp), "perft bp");
  QWC_TRUE(na == nb, "count independent of repetition history");
  QWC_TRUE(na == nbp, "count independent of high counters");
  return qwc_end();
}

int main(int argc, char **argv) {
  const bool full = (argc >= 2) && (std::strcmp(argv[1], "--full") == 0);
  int fail = 0;
  fail |= t_counts(full);
  fail |= t_terminal();
  fail |= t_invalid_depth();
  fail |= t_divide();
  fail |= t_determinism();
  fail |= t_preservation();
  fail |= t_independence();
  if (fail) {
    std::printf("perft test: FAIL\n");
    return 1;
  }
  std::printf("perft test: all groups PASS (%s)\n", full ? "full depths" : "bounded depths");
  return 0;
}
