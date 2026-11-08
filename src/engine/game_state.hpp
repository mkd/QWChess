/* QwenChess engine: the owned game state (the one authoritative Position + its
 * complete, stably-allocated repetition history).
 *
 * Ownership / lifetime:
 *  - A GameState OWNS its undo-record storage (std::vector<StateInfo>) and holds the
 *    authoritative Position. It is held exclusively through a std::unique_ptr by
 *    exactly one owner at a time (the UciEngine installs exactly one game state).
 *  - It is NON-copyable and NON-movable by value: the Position's `history` pointer
 *    and every StateInfo's `prev` pointer borrow the record storage. Copying or
 *    moving the object by value could dangle or duplicate those links; the ONLY safe
 *    transfer is the owning pointer (unique_ptr).
 *  - Record addresses are STABLE for the whole life of the object: the storage is
 *    resized to the final count before the first make and is never resized again
 *    (a built state is immutable -- a new command builds a NEW state).
 *
 * Construction is TRANSACTIONAL: setup (startpos or FEN) + replay of the complete
 * UCI move line into the final-sized record array. Any failure (a bad FEN, a token
 * that is not a legal move of the running position, or a replay longer than the
 * bound) returns nullptr and installs nothing; the caller keeps its previous state.
 * A successfully loaded FEN / startpos starts a FRESH history (Position.history ==
 * nullptr), and each replayed record links into the chain in order, so the installed
 * state carries a complete, stable history that the repetition policy can query.
 */
#ifndef QWC_ENGINE_GAME_STATE_HPP
#define QWC_ENGINE_GAME_STATE_HPP

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "position/position.hpp"
#include "position/state.hpp"

namespace qwc {

class GameState {
 public:
  GameState() = default;
  GameState(const GameState&) = delete;          /* the history links are non-owning */
  GameState& operator=(const GameState&) = delete;
  GameState(GameState&&) = delete;               /* never moved; transfer the pointer */
  GameState& operator=(GameState&&) = delete;

  /* Hard bound on the length of one replayed move line (the complete UCI `position`
   * history): 8192 plies. A longer line is rejected outright (the previous state is
   * kept); it is far beyond any real game and bounds the owned record storage
   * (8192 * sizeof(StateInfo)). */
  static constexpr std::size_t kMaxReplayPlies = 8192;

  /* A fresh state: the standard start position (+ a replayed move line). Returns
   * nullptr (nothing built) if any move token is not legal for the running
   * position or the line exceeds kMaxReplayPlies. */
  static std::unique_ptr<GameState>
  build_startpos(std::span<const std::string_view> moves);

  /* A fresh state: a standard 6-field FEN (+ a replayed move line). Returns nullptr
   * (nothing built) on a rejected FEN, an illegal move token, or a line longer than
   * kMaxReplayPlies. `error` (optional) receives a static-backed reason on failure. */
  static std::unique_ptr<GameState>
  build_fen(std::string_view fen, std::span<const std::string_view> moves,
            std::string_view* error = nullptr);

  /* The authoritative position. Its `history` pointer borrows this object's record
   * storage (stable addresses) and is read-only for search jobs. */
  const Position& pos() const { return pos_; }

  /* The number of replayed moves in the installed history (0 for a bare setup). */
  std::size_t ply_count() const { return count_; }

 private:
  /* The shared transactional builder (defined in game_state.cpp). `from_startpos`
   * selects the setup; otherwise `fen` (a standard 6-field FEN) is parsed. */
  static std::unique_ptr<GameState>
  build(bool from_startpos, std::string_view fen,
        std::span<const std::string_view> moves, std::string_view* error);

  Position pos_{};                /* pos_.history -> the latest record (borrowed) */
  std::vector<StateInfo> records_;/* stable addresses; resized once to the final count */
  std::size_t count_ = 0;         /* records in use (== records_.size()) */
};

}  // namespace qwc

#endif /* QWC_ENGINE_GAME_STATE_HPP */
