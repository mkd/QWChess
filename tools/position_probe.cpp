/* QwenChess: position_probe -- the independent move-generation differential oracle.
 *
 * A small, test-only C++ program that links QWChess and is driven by
 * tools/movegen_diff.py (which uses the pinned python `chess` 1.11.2 to compute the
 * expected moves independently). It is NOT shipped engine code and NOT a UCI parser.
 *
 * Protocol (stdin):
 *   <N>                      the number of lines to process
 *   <root_fen>               (repeated N times)
 *   <m1> <m2> ... <mK>       a bounded line of ENCODED moves (the QWChess u32 layout);
 *                            may be empty (just the root position)
 *
 * For each line the probe loads the root FEN, reports it, applies each move in order
 * (forward), and reports every resulting position; it then unwinds the whole line in
 * LIFO order and verifies every restoration. For each reported position it emits:
 *   P <line> <ply> <fen> <m1> <m2> ... <mN>
 * where <ply> is 0 for the root and 1..K for the forward positions, <fen> is the
 * standard six-field FEN (the recorded en-passant square, per the QWChess convention),
 * and the trailing moves are the COMPLETE encoded legal set (unsigned decimal) in the
 * engine's deterministic order.
 *
 * For every applied move the probe also verifies the child: the representation
 * validator passes, the original mover's king is safe, and a cold rebuild of the
 * derived caches + key agrees with the incrementally-updated values. Any malformed
 * request, a make/unmake that fails, a child that fails verification, or a restoration
 * mismatch is a nonzero exit, with a reproducible diagnostic naming the line and ply.
 *
 * This verifies make/unmake restoration, the FEN convention and the complete legal
 * set against the independent reference; it performs no search, evaluation or
 * pruning.
 */
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <span>
#include <string>
#include <string_view>

#include "core/types.hpp"
#include "movegen/movegen.hpp"
#include "position/fen.hpp"
#include "position/position.hpp"
#include "position/state.hpp"

namespace qwc {
namespace {

// Bounded line length (the checker's walks are capped well below this).
constexpr int kMaxMoves = 128;
// A conservative bound on the legal moves in one orthodox position (16 pieces x 27
// destinations + 2 en-passant + 2 castles = 436); the true maximum is well below it.
constexpr int kMoveCapacity = 16 * 27 + 2 + 2;
constexpr int kBufFen = 256;
constexpr int kBufLine = 8192;

/* Reproducible diagnostic (line/ply context) + nonzero exit. */
[[noreturn]] void fail(int code, int line, int ply, const char *reason) {
  std::fprintf(stderr, "position_probe: line %d ply %d: %s\n", line, ply, reason);
  std::exit(code);
}

/* Parse one move token (unsigned decimal or 0x-hex) into a Move. Returns 0 on a
 * malformed token. */
static bool parse_move(const char *tok, Move *out) {
  if (!tok || !*tok) return false;
  char *end = nullptr;
  const unsigned long long v = std::strtoull(tok, &end, 0);   // base 0 -> dec or 0x-hex
  if (end == tok || *end != '\0') return false;
  if (v > 0x7FFFFu) return false;   // above the used move bits -> malformed
  *out = static_cast<Move>(v);
  return true;
}

/* Full logical Position equality (board, scalars, derived caches, key, history head).
 * Struct padding is not logical state. */
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

/* Requirement 6: verify an applied child -- the validator, the original mover's king
 * safety, and a cold rebuild that must agree with the incrementally-updated derived
 * caches + key. */
static void verify_child(const Position &child, int line, int ply) {
  const char *why = nullptr;
  if (!pos_validate(&child, &why)) fail(3, line, ply, "child failed the representation validator");
  // The mover is the side that is no longer to move after the applied move.
  const Color mover = (child.side == Color::WHITE) ? Color::BLACK : Color::WHITE;
  if (pos_is_in_check(&child, mover)) fail(3, line, ply, "applied move leaves the mover's king in check");
  // Cold rebuild of the derived caches + key must agree with the incremental update.
  Position rebuilt = child;
  pos_rebuild(&rebuilt);
  for (int p = 0; p < PIECE_NB; p++)
    if (rebuilt.by_piece[p] != child.by_piece[p]) fail(3, line, ply, "cold rebuild by_piece disagrees");
  for (int c = 0; c < COLOR_NB; c++)
    if (rebuilt.by_color[c] != child.by_color[c]) fail(3, line, ply, "cold rebuild by_color disagrees");
  if (rebuilt.occ != child.occ) fail(3, line, ply, "cold rebuild occupancy disagrees");
  if (rebuilt.white_king_sq != child.white_king_sq) fail(3, line, ply, "cold rebuild white king disagrees");
  if (rebuilt.black_king_sq != child.black_king_sq) fail(3, line, ply, "cold rebuild black king disagrees");
  if (rebuilt.key != child.key) fail(3, line, ply, "cold rebuild key disagrees");
}

/* Report one position: its FEN (as six field-tokens) + the complete encoded legal
 * set (unsigned decimal). The FEN's internal spaces would otherwise break the checker
 * whitespace split, so each of the six standard fields is emitted as its own token. */
static void report(const Position &pos, int line, int ply) {
  char fen[kBufFen] = {0};
  if (!fen_emit(pos, std::span(fen, kBufFen))) fail(3, line, ply, "fen_emit failed");
  std::array<Move, kMoveCapacity> moves{};
  std::size_t count = 0;
  if (!movegen_legal(pos, moves, count)) fail(3, line, ply, "movegen_legal failed");
  std::printf("P %d %d", line, ply);
  std::istringstream iss(fen);
  std::string field;
  while (iss >> field)
    std::printf(" %s", field.c_str());
  for (std::size_t i = 0; i < count; i++)
    std::printf(" %llu", static_cast<unsigned long long>(moves[i]));
  std::printf("\n");
}

/* Process one line (a root FEN + a bounded move list). Returns 0 on success. */
int process_line(int line, const char *fen, const char *moves) {
  Position pos;
  std::string_view err;
  if (!fen_load(pos, std::string_view(fen), &err)) {
    std::fprintf(stderr, "position_probe: line %d: cannot load FEN \"%s\" (%s)\n",
                 line, fen, err.empty() ? std::string_view("no reason").data() : err.data());
    std::exit(2);
  }

  // Parse the (bounded) move list.
  std::array<Move, kMaxMoves> mv{};
  int k = 0;
  {
    char buf[kBufLine];
    std::snprintf(buf, sizeof buf, "%s", moves ? moves : "");
    for (char *tok = std::strtok(buf, " \t"); tok; tok = std::strtok(nullptr, " \t")) {
      if (k >= kMaxMoves) fail(2, line, -1, "move line too long");
      if (!parse_move(tok, &mv[k])) fail(2, line, -1, "malformed move token");
      k++;
    }
  }

  std::array<StateInfo, kMaxMoves> undo{};
  std::array<Position, kMaxMoves> expect{};   // test-only saved restoration expectations

  const u64 root_key = pos.key;
  const u8 root_side = to_under(pos.side);
  const u8 root_cr = pos.cr;
  const Square root_ep = pos.ep_sq;
  const u16 root_hm = pos.halfmove;
  const u16 root_fm = pos.fullmove;
  std::array<Piece, SQ_NB> root_mailbox = pos.mailbox;
  const u64 root_occ = pos.occ;

  report(pos, line, 0);

  for (int i = 0; i < k; i++) {
    expect[i] = pos;   // the pre-move state the matching unmake must restore
    if (!pos_make_move(&pos, &undo[i], mv[i])) fail(3, line, i + 1, "make failed");
    verify_child(pos, line, i + 1);
    report(pos, line, i + 1);
  }

  // Unwind the whole line in LIFO order, verifying every restoration.
  for (int i = k - 1; i >= 0; i--) {
    pos_unmake_move(&pos, &undo[i]);
    if (!pos_logical_equal(&pos, &expect[i])) fail(4, line, i + 1, "unmake did not restore the position");
  }

  // The root must be restored exactly (board + scalars; the history head returns to
  // nullptr since the probe's makes used the local undo records).
  if (pos.key != root_key || to_under(pos.side) != root_side || pos.cr != root_cr ||
      pos.ep_sq != root_ep || pos.halfmove != root_hm || pos.fullmove != root_fm ||
      pos.occ != root_occ)
    fail(4, line, -1, "the root position was not restored");
  for (int s = 0; s < SQ_NB; s++)
    if (pos.mailbox[s] != root_mailbox[s]) fail(4, line, -1, "the root mailbox was not restored");
  return 0;
}

}  // namespace
}  // namespace qwc

int main() {
  using namespace qwc;

  char buf[kBufLine];
  if (!std::fgets(buf, sizeof buf, stdin)) fail(2, -1, -1, "expected a line count");
  const int n = std::atoi(buf);
  if (n <= 0 || n > 100000) fail(2, -1, -1, "bad line count");

  for (int line = 0; line < n; line++) {
    char fen[kBufFen] = {0};
    char moves[kBufLine] = {0};
    if (!std::fgets(fen, sizeof fen, stdin)) fail(2, line, -1, "missing FEN line");
    if (!std::fgets(moves, sizeof moves, stdin)) fail(2, line, -1, "missing moves line");
    fen[strcspn(fen, "\n")] = '\0';
    moves[strcspn(moves, "\n")] = '\0';
    process_line(line, fen, moves);   // exits on any failure
  }
  return 0;
}
