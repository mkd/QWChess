/* QwenChess engine: the owned game state (implementation).
 *
 * See game_state.hpp for the ownership / lifetime contract. The builder is the one
 * transactional code path for all state construction: a scratch Position + a record
 * array sized to the FINAL replay length, then setup + replay; the owning pointer is
 * transferred only on full success.
 */
#include "engine/game_state.hpp"

#include "engine/uci_codec.hpp"
#include "position/fen.hpp"

namespace qwc {

/* The shared builder. `from_startpos` selects the setup; otherwise `fen` (a standard
 * 6-field FEN) is parsed. On ANY failure (a rejected FEN, a move token that is not
 * legal for the running position, a line longer than the bound) it returns nullptr
 * without having published anything. `error` receives a static-backed reason. */
std::unique_ptr<GameState>
GameState::build(bool from_startpos, std::string_view fen,
                 std::span<const std::string_view> moves, std::string_view* error) {
  if (moves.size() > GameState::kMaxReplayPlies) {
    if (error) *error = "move line exceeds the replay bound";
    return nullptr;
  }

  auto gs = std::unique_ptr<GameState>(new GameState());
  gs->pos_ = Position{};   /* value-initialized scratch; the setup fills every field */

  if (from_startpos) {
    pos_set_startpos(&gs->pos_);   /* starts a fresh history (Position.history == nullptr) */
  } else if (!fen_load(gs->pos_, fen, error)) {
    return nullptr;   /* the parser left the scratch untouched; discard it */
  }

  /* Size the record storage ONCE to the final count: every record address is stable
   * for the whole life of the object, so the Position.history / StateInfo.prev links
   * never dangle. `count_` only advances within it (never resizes). */
  gs->records_.resize(moves.size());
  gs->count_ = 0;

  for (std::size_t i = 0; i < moves.size(); ++i) {
    Move m;
    if (!uci_move_parse(gs->pos_, moves[i], m)) {
      if (error) *error = "a move token is not legal for the running position";
      return nullptr;
    }
    if (!pos_make_move(&gs->pos_, &gs->records_[i], m)) {
      if (error) *error = "a move could not be applied";
      return nullptr;
    }
    gs->count_ = i + 1;
  }
  return gs;
}

std::unique_ptr<GameState>
GameState::build_startpos(std::span<const std::string_view> moves) {
  return build(true, std::string_view{}, moves, nullptr);
}

std::unique_ptr<GameState>
GameState::build_fen(std::string_view fen, std::span<const std::string_view> moves,
                     std::string_view* error) {
  return build(false, fen, moves, error);
}

}  // namespace qwc
