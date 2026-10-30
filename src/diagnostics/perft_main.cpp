/* QwenChess: qwcperft -- a developer diagnostic executable (NOT the engine).
 *
 * Usage:
 *   qwcperft <fen> <depth>           print the perft node count at `depth`
 *   qwcperft --divide <fen> <depth>  print one line per root move (move + subtree
 *                                    count) plus the total; requires depth >= 1
 *
 * It loads a FEN, runs the shared perft() traversal, and (for --divide) applies each
 * root move to count the child. It is a diagnostic: it evaluates nothing and chooses
 * no best move. The --divide form preserves its input position (verified: the FEN and
 * key before the traversal equal those after) and its subtree lines sum to the
 * ordinary perft at that depth.
 *
 * Move notation: <from><to> using a-h / 1-8 (internal coordinates), with a flag tag:
 *   +c  castle -- the internal king->rook ORIGIN encoding (e1h1 short / e1a1 long)
 *   +e  en-passant capture
 *   =N  =B  =R  =Q   promotion to that piece
 * Exit codes: 0 ok; 2 bad usage / FEN; 3 a traversal or make failure.
 */
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "core/types.hpp"
#include "diagnostics/perft.hpp"
#include "movegen/movegen.hpp"
#include "position/fen.hpp"

namespace qwc {
namespace {

void square_to_str(Square sq, char out[3]) {
  out[0] = static_cast<char>('a' + (sq & 7));   // file a..h
  out[1] = static_cast<char>('1' + (sq >> 3));  // rank 1..8
  out[2] = '\0';
}

void move_to_str(Move m, char out[8]) {
  char f[3], t[3];
  square_to_str(move_from(m), f);
  square_to_str(move_to(m), t);
  int n = 0;
  out[n++] = f[0]; out[n++] = f[1]; out[n++] = t[0]; out[n++] = t[1];
  if (is_castle(m)) {
    out[n++] = '+'; out[n++] = 'c';
  } else if (is_ep(m)) {
    out[n++] = '+'; out[n++] = 'e';
  } else if (is_promotion(m)) {
    out[n++] = '=';
    switch (move_promo(m)) {
      case 2: out[n++] = 'N'; break;
      case 3: out[n++] = 'B'; break;
      case 4: out[n++] = 'R'; break;
      case 5: out[n++] = 'Q'; break;
      default: out[n++] = '?'; break;
    }
  }
  out[n] = '\0';
}

bool load_position(const char* fen, Position& pos) {
  std::string_view err;
  return fen_load(pos, std::string_view(fen), &err);
}

}  // namespace
}  // namespace qwc

int main(int argc, char** argv) {
  using namespace qwc;

  if (argc == 3) {
    // perft mode
    const int depth = std::atoi(argv[2]);
    Position pos;
    if (!load_position(argv[1], pos)) {
      std::fprintf(stderr, "qwcperft: cannot load FEN: %s\n", argv[1]);
      return 2;
    }
    u64 nodes = 0;
    if (!perft(pos, depth, nodes)) {
      std::fprintf(stderr, "qwcperft: perft failed (depth %d)\n", depth);
      return 3;
    }
    std::printf("%llu\n", static_cast<unsigned long long>(nodes));
    return 0;
  }

  if (argc == 4 && std::strcmp(argv[1], "--divide") == 0) {
    const int depth = std::atoi(argv[3]);
    if (depth < 1 || depth > MAX_PLY) {
      std::fprintf(stderr, "qwcperft --divide: depth must be in [1, %d]\n", MAX_PLY);
      return 2;
    }
    Position pos;
    if (!load_position(argv[2], pos)) {
      std::fprintf(stderr, "qwcperft: cannot load FEN: %s\n", argv[2]);
      return 2;
    }

    // Capture the input's presentation (FEN + key) to verify the divide preserves it.
    char fen_before[FEN_BUF_SIZE] = {0};
    if (!fen_emit(pos, std::span(fen_before, FEN_BUF_SIZE))) {
      std::fprintf(stderr, "qwcperft: cannot emit FEN\n");
      return 3;
    }
    const u64 key_before = pos_key(&pos);

    std::array<Move, kPerftMoveCapacity> moves{};
    std::size_t count = 0;
    if (!movegen_legal(pos, moves, count)) {
      std::fprintf(stderr, "qwcperft --divide: move generation failed\n");
      return 3;
    }

    u64 total = 0;
    char s[8];
    for (std::size_t i = 0; i < count; ++i) {
      move_to_str(moves[i], s);
      StateInfo rec{};
      if (!pos_make_move(&pos, &rec, moves[i])) {
        std::fprintf(stderr, "qwcperft --divide: make failed for %s\n", s);
        return 3;
      }
      u64 sub = 0;
      const bool ok = perft(pos, depth - 1, sub);
      pos_unmake_move(&pos, &rec);   // always undo, success or failure
      if (!ok) {
        std::fprintf(stderr, "qwcperft --divide: perft failed for child of %s\n", s);
        return 3;
      }
      if (sub > std::numeric_limits<u64>::max() - total) {
        std::fprintf(stderr, "qwcperft --divide: total overflow\n");
        return 3;
      }
      total += sub;
      std::printf("%s %llu\n", s, static_cast<unsigned long long>(sub));
    }
    std::printf("total %llu\n", static_cast<unsigned long long>(total));

    // Preserve input state: the FEN and key must be unchanged after the traversal.
    char fen_after[FEN_BUF_SIZE] = {0};
    if (!fen_emit(pos, std::span(fen_after, FEN_BUF_SIZE)) ||
        std::strcmp(fen_before, fen_after) != 0 || pos_key(&pos) != key_before) {
      std::fprintf(stderr, "qwcperft --divide: input position was not preserved\n");
      return 3;
    }
    return 0;
  }

  std::fprintf(stderr,
               "usage: qwcperft <fen> <depth> | qwcperft --divide <fen> <depth> "
               "(depth in [0, %d])\n", MAX_PLY);
  return 2;
}
