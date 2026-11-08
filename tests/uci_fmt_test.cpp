/* QWChess test: the UCI protocol module (score presentation, go parsing/resolution).
 *
 * White-box tests of the protocol layer's independently testable surface:
 *  - format_score / mate_moves: the exact text a real score becomes on the wire,
 *    and the refusal of sentinel values;
 *  - parse_go: accepted forms, rejected forms (bounds, signs, overflow, unknown /
 *    unsupported modes), and the bare-`go` case;
 *  - resolve_go: explicit zero budgets, the documented clock allocation, mixed
 *    deadlines (the tighter wins), side selection from the installed root, and the
 *    move-overhead option.
 *
 * Links the real engine library objects (the actual module under test).
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "engine/uci.hpp"
#include "position/fen.hpp"
#include "position/position.hpp"
#include "test_util.hpp"

using namespace qwc;

namespace {
/* Stable backing for the string_view tokens: a fixed slot array (a growing
 * std::vector<std::string> would relocate each string's SSO buffer on reallocation
 * and dangle the earlier views). Slots are never freed, which is fine for a test. */
constexpr int kSlots = 96;
char slot_storage[kSlots][32];
int slot_n = 0;
std::vector<std::string_view> mk(std::initializer_list<const char*> items) {
  std::vector<std::string_view> out;
  for (const char* s : items) {
    QWC_TRUE(slot_n < kSlots && std::strlen(s) < 32, "token fits a backing slot");
    std::strcpy(slot_storage[slot_n], s);
    out.push_back(std::string_view(slot_storage[slot_n++]));
  }
  return out;
}
bool parse_into(std::initializer_list<const char*> args, GoLimits& gl) {
  const auto v = mk(args);
  return UciEngine::parse_go(std::span<const std::string_view>(v.data(), v.size()), gl);
}
Position startpos() {
  Position p{};
  pos_set_startpos(&p);
  return p;
}
Position black_to_move() {
  Position p{};
  const char* fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1";
  QWC_TRUE(fen_load(p, fen, nullptr), "black-to-move FEN loads");
  return p;
}
}  // namespace

int main() {
  qwc_begin("uci-fmt");

  /* ============================ score presentation ============================ */
  std::string s;
  QWC_TRUE(UciEngine::format_score(mate_in(1), s) && s == "mate 1", "mate_in(1) -> 'mate 1'");
  QWC_TRUE(UciEngine::format_score(mate_in(3), s) && s == "mate 2", "mate_in(3) -> 'mate 2'");
  QWC_TRUE(UciEngine::format_score(mate_in(256), s) && s == "mate 128", "mate_in(256) -> 'mate 128'");
  QWC_TRUE(UciEngine::format_score(mated_in(0), s) && s == "mate 0", "mated_in(0) -> 'mate 0'");
  QWC_TRUE(UciEngine::format_score(mated_in(2), s) && s == "mate -1", "mated_in(2) -> 'mate -1'");
  QWC_TRUE(UciEngine::format_score(mated_in(4), s) && s == "mate -2", "mated_in(4) -> 'mate -2'");
  QWC_TRUE(UciEngine::format_score(0, s) && s == "cp 0", "0 -> 'cp 0'");
  QWC_TRUE(UciEngine::format_score(123, s) && s == "cp 123", "123 -> 'cp 123'");
  QWC_TRUE(UciEngine::format_score(-456, s) && s == "cp -456", "-456 -> 'cp -456'");
  QWC_TRUE(UciEngine::format_score(VALUE_TB, s) && s == "cp 30743", "TB is a cp value, not mate");
  QWC_TRUE(UciEngine::format_score(-VALUE_TB_WIN_IN_MAX_PLY, s) && s == "cp -30487", "-TB_WIN is a cp value");
  /* Sentinels are never presented: format_score must refuse them. */
  QWC_TRUE(!UciEngine::format_score(VALUE_INFINITE, s) && s.empty(), "+INFINITE refused");
  QWC_TRUE(!UciEngine::format_score(-VALUE_INFINITE, s) && s.empty(), "-INFINITE refused");
  QWC_TRUE(!UciEngine::format_score(VALUE_NONE, s) && s.empty(), "+NONE refused");
  QWC_TRUE(!UciEngine::format_score(-VALUE_NONE, s) && s.empty(), "-NONE refused");

  /* The signed UCI move distance. */
  QWC_EQ_I(UciEngine::mate_moves(mate_in(1)), 1, "mate_moves(mate_in(1))");
  QWC_EQ_I(UciEngine::mate_moves(mate_in(3)), 2, "mate_moves(mate_in(3))");
  QWC_EQ_I(UciEngine::mate_moves(mated_in(2)), -1, "mate_moves(mated_in(2))");
  QWC_EQ_I(UciEngine::mate_moves(mated_in(4)), -2, "mate_moves(mated_in(4))");
  QWC_EQ_I(UciEngine::mate_moves(mated_in(0)), 0, "mate_moves(mated_in(0))");
  QWC_EQ_I(UciEngine::mate_moves(0), 0, "mate_moves of a draw is 0");
  QWC_EQ_I(UciEngine::mate_moves(VALUE_TB), 0, "mate_moves of a TB value is 0");

  /* ================================ parse_go ================================ */
  GoLimits gl;

  QWC_TRUE(parse_into({}, gl) && gl.has_depth == false && gl.infinite == false
           && gl.valid, "bare go is valid and limit-free");
  QWC_TRUE(parse_into({"depth", "8"}, gl) && gl.has_depth && gl.depth == 8, "depth 8");
  QWC_TRUE(!parse_into({"depth", "0"}, gl) && !gl.valid && !gl.invalid_why.empty(), "depth 0 rejected");
  QWC_TRUE(parse_into({"depth", "256"}, gl) && gl.depth == 256, "depth 256 accepted");
  QWC_TRUE(!parse_into({"depth", "257"}, gl), "depth 257 rejected (bounded by MAX_PLY)");
  QWC_TRUE(!parse_into({"depth", "-5"}, gl), "negative depth rejected");
  QWC_TRUE(!parse_into({"depth", "abc"}, gl), "non-numeric depth rejected");
  QWC_TRUE(!parse_into({"depth"}, gl), "depth without a value rejected");
  QWC_TRUE(parse_into({"nodes", "100"}, gl) && gl.has_nodes && gl.nodes == 100, "nodes 100");
  QWC_TRUE(parse_into({"nodes", "0"}, gl) && gl.has_nodes && gl.nodes == 0, "nodes 0 parses (budget decided later)");
  QWC_TRUE(!parse_into({"nodes", "99999999999999999999"}, gl), "overflowing nodes rejected");
  QWC_TRUE(!parse_into({"nodes", "-3"}, gl), "negative nodes rejected");
  QWC_TRUE(parse_into({"movetime", "0"}, gl) && gl.has_movetime && gl.movetime == 0, "movetime 0 parses");
  QWC_TRUE(parse_into({"wtime", "30000", "btime", "30000"}, gl) && gl.has_wtime && gl.has_btime, "clock times");
  QWC_TRUE(!parse_into({"wtime", "-1"}, gl), "negative wtime rejected");
  QWC_TRUE(parse_into({"movestogo", "10"}, gl) && gl.movestogo == 10, "movestogo 10");
  QWC_TRUE(!parse_into({"movestogo", "0"}, gl), "movestogo 0 rejected");
  QWC_TRUE(parse_into({"infinite"}, gl) && gl.infinite, "infinite flag");
  QWC_TRUE(!parse_into({"ponder"}, gl) && gl.invalid_why.find("ponder") != std::string::npos, "ponder rejected explicitly");
  QWC_TRUE(!parse_into({"mate", "5"}, gl) && gl.invalid_why.find("mate") != std::string::npos, "mate rejected explicitly");
  QWC_TRUE(!parse_into({"searchmoves", "e2e4"}, gl), "searchmoves rejected explicitly");
  QWC_TRUE(!parse_into({"bogus", "3"}, gl), "unknown limit rejected");
  QWC_TRUE(parse_into({"depth", "8", "nodes", "100", "movetime", "500", "wtime", "30000",
                       "btime", "30000", "winc", "100", "binc", "100", "movestogo", "10"}, gl)
           && gl.has_depth && gl.has_nodes && gl.has_movetime && gl.has_wtime && gl.has_binc
           && gl.has_movestogo, "a fully mixed go parses");

  /* ================================ resolve_go ================================ */
  const Position w = startpos();
  const Position b = black_to_move();
  SearchLimits lim;
  bool zb = false;

  /* Bare go -> infinite, no caps. */
  gl = GoLimits{};
  UciEngine::resolve_go(gl, w, 0, lim, zb);
  QWC_TRUE(!zb && lim.infinite && lim.depth == 0 && lim.max_nodes == 0 && !lim.has_deadline,
           "bare go resolves to a plain infinite job");

  /* depth only -> finite job, no deadline. */
  gl = GoLimits{}; gl.has_depth = true; gl.depth = 8;
  UciEngine::resolve_go(gl, w, 0, lim, zb);
  QWC_TRUE(!zb && !lim.infinite && lim.depth == 8 && !lim.has_deadline, "depth-only resolves finite");

  /* An explicit ZERO budget (nodes) -> immediate-fallback job, no internal caps. */
  gl = GoLimits{}; gl.has_nodes = true; gl.nodes = 0;
  UciEngine::resolve_go(gl, w, 0, lim, zb);
  QWC_TRUE(zb && lim.depth == 0 && lim.max_nodes == 0 && !lim.has_deadline && !lim.infinite,
           "nodes 0 resolves to a zero-budget job");

  gl = GoLimits{}; gl.has_movetime = true; gl.movetime = 0;
  UciEngine::resolve_go(gl, w, 0, lim, zb);
  QWC_TRUE(zb, "movetime 0 resolves to a zero-budget job");

  /* Zero usable clock time (side to move white, wtime 0) -> zero budget. */
  gl = GoLimits{}; gl.has_wtime = true; gl.has_btime = true; gl.wtime = 0; gl.btime = 10000;
  UciEngine::resolve_go(gl, w, 0, lim, zb);
  QWC_TRUE(zb, "wtime 0 resolves to a zero-budget job");

  /* The documented 3x-movestogo allocation plus increment, capped by usable time. */
  gl = GoLimits{}; gl.has_wtime = true; gl.has_btime = true; gl.wtime = 10000; gl.btime = 10000;
  gl.has_movestogo = true; gl.movestogo = 4;
  const TimePoint t0 = clock_now();
  UciEngine::resolve_go(gl, w, 0, lim, zb);
  const auto got = std::chrono::duration_cast<std::chrono::milliseconds>(lim.deadline - t0).count();
  QWC_TRUE(!zb && lim.has_deadline, "a clock go gets a deadline");
  QWC_TRUE(got >= 7498 && got <= 7560, "10000ms/4-movestogo allocates 7500ms (3x share, no inc)");

  /* Black to move: the BLACK fields are the side's time. */
  gl = GoLimits{}; gl.has_wtime = true; gl.has_btime = true; gl.has_winc = true; gl.has_binc = true;
  gl.wtime = 1000; gl.btime = 1000; gl.winc = 99999; gl.binc = 50; gl.has_movestogo = true; gl.movestogo = 5;
  const TimePoint tb = clock_now();
  UciEngine::resolve_go(gl, b, 0, lim, zb);
  const auto gotb = std::chrono::duration_cast<std::chrono::milliseconds>(lim.deadline - tb).count();
  QWC_TRUE(!zb && lim.has_deadline && gotb >= 590 && gotb <= 665,
           "black-to-move uses btime/binc (650ms), not the white fields");

  /* A requested clock whose side field is absent -> exhausted (safe), never unbounded. */
  gl = GoLimits{}; gl.has_wtime = true; gl.wtime = 1000;   /* only wtime; root is BLACK to move */
  UciEngine::resolve_go(gl, b, 0, lim, zb);
  QWC_TRUE(zb && !lim.has_deadline && !lim.infinite, "absent side-time resolves to a zero-budget job");

  /* The move-overhead option eats into the usable time. */
  gl = GoLimits{}; gl.has_wtime = true; gl.has_btime = true; gl.wtime = 1000; gl.btime = 1000;
  UciEngine::resolve_go(gl, w, 2000, lim, zb);
  QWC_TRUE(zb, "overhead larger than the remaining time resolves to a zero-budget job");

  /* A mixed deadline: the tighter of movetime and the clock allocation wins. */
  gl = GoLimits{}; gl.has_movetime = true; gl.movetime = 5000;
  gl.has_wtime = true; gl.has_btime = true; gl.wtime = 1000; gl.btime = 1000; gl.has_movestogo = true; gl.movestogo = 4;
  const TimePoint tm = clock_now();
  UciEngine::resolve_go(gl, w, 0, lim, zb);
  const auto gotm = std::chrono::duration_cast<std::chrono::milliseconds>(lim.deadline - tm).count();
  QWC_TRUE(lim.has_deadline && gotm >= 690 && gotm <= 755, "mixed limits take the tighter cap (750ms)");

  /* `go infinite movetime 0`: the exhausted cap wins over the infinite flag. */
  gl = GoLimits{}; gl.infinite = true; gl.has_movetime = true; gl.movetime = 0;
  UciEngine::resolve_go(gl, w, 0, lim, zb);
  QWC_TRUE(zb && !lim.infinite, "an exhausted cap inside an infinite go resolves to a finite fallback job");

  return qwc_end() ? 1 : 0;
}
