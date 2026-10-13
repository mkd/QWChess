/* QwenChess entry point (scaffold).
 *
 * This binary identifies itself as a build scaffold only. It is NOT a playable
 * UCI chess engine and must not be presented as one. It prints the frozen
 * foundational constants and exits. Real engine behaviour lands in later tasks. */
#include <cstdio>
#include "core/types.hpp"

int main(void) {
  std::printf("QwenChess scaffold (not a playable engine)\n");
  std::printf("  host       : %d-bit\n", (int)(8 * sizeof(unsigned long long)));
  std::printf("  MAX_PLY    : %d  (stack bound %d)\n", MAX_PLY, MAX_PLY_STACK);
  std::printf("  score      : MATE=%d  MATE_IN_MAX=%d  TB_WIN=%d  INF=%d  NONE=%d\n",
         VALUE_MATE, VALUE_MATE_IN_MAX_PLY, VALUE_TB_WIN_IN_MAX_PLY,
         VALUE_INFINITE, VALUE_NONE);
  std::printf("  no-square  : %d\n", NO_SQUARE);
  return 0;
}
