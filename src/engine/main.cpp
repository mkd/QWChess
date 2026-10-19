/* QwenChess entry point (scaffold).
 *
 * This binary identifies itself as a build scaffold only. It is NOT a playable
 * UCI chess engine and must not be presented as one. It prints the frozen
 * foundational constants and exits. Real engine behaviour (UCI, search) lands in
 * later tasks. */
#include <climits>   /* CHAR_BIT (for the pointer-width diagnostic below) */
#include <iostream>
#include "core/types.hpp"

int main() {
  std::cout << "QwenChess scaffold (not a playable engine)\n";
  /* Pointer width in bits, labelled accurately as the width of a pointer (not "host
   * bits" from an arbitrary integer type): sizeof(void*) * CHAR_BIT. */
  std::cout << "  pointer width: " << sizeof(void*) * CHAR_BIT << "-bit\n";
  std::cout << "  MAX_PLY      : " << qwc::MAX_PLY << "  (stack bound " << qwc::MAX_PLY_STACK << ")\n";
  std::cout << "  score        : MATE=" << qwc::VALUE_MATE
            << "  MATE_IN_MAX=" << qwc::VALUE_MATE_IN_MAX_PLY
            << "  TB_WIN=" << qwc::VALUE_TB_WIN_IN_MAX_PLY
            << "  INF=" << qwc::VALUE_INFINITE
            << "  NONE=" << qwc::VALUE_NONE << "\n";
  /* NO_SQUARE is byte-sized (Square is a u8 = unsigned char); stream an integer so
   * it prints as "64" rather than as the character that byte 64 denotes. */
  std::cout << "  no-square    : " << static_cast<int>(qwc::NO_SQUARE) << "\n";
  return 0;
}
