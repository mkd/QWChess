/* QwenChess: entry point.
 *
 * A playable, standalone UCI chess engine: a deterministic single search worker,
 * labelled temporary Material static evaluation (search plumbing only), no
 * transposition table and no NNUE yet (see docs/PLAN.md T007b).
 *
 * Normal operation prints NO banner at startup: the engine starts directly in the
 * UCI command loop on stdin/stdout. The only startup argument is `--version`,
 * which prints the engine version and exits.
 *
 * The immutable shared lookup tables (attacks, Zobrist) are initialized ONCE on
 * the main thread before the protocol loop -- and therefore before any worker
 * thread can start: the tables' lazy first-use flags are not synchronization, so
 * an explicit one-time initialization is the documented start point.
 */
#include <iostream>
#include <string_view>

#include "core/attacks.hpp"
#include "core/zobrist.hpp"
#include "engine/uci.hpp"

int main(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "--version") {
    std::cout << qwc::UciEngine::kName << " " << qwc::UciEngine::kVersion << "\n";
    return 0;
  }

  qwc::attacks_init();   /* immutable, shared, read-only after this point */
  qwc::zobrist_init();

  qwc::UciEngine engine;
  return engine.run(std::cin, std::cout);
}
