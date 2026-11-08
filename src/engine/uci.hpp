/* QwenChess engine: the UCI protocol engine (implementation module).
 *
 * A playable, standalone UCI chess engine with a deterministic single search worker:
 *  - the command READER runs on the main thread and NEVER blocks in a search;
 *  - a single search worker thread runs do_search() on an owned root copy;
 *  - the engine installs exactly ONE owned GameState (the authoritative Position +
 *    its complete, stable repetition history); a search job borrows that immutable
 *    pre-root history and owns a mutable root copy.
 *
 * Responsiveness / cancellation contract:
 *  - while a job is active, `isready` answers a flushed `readyok` and does NOT stop
 *    or join the job; `stop` only requests cancellation (an atomic flag, plus a
 *    condition-variable notification coordinated on the same mutex the worker waits
 *    on, so a notification is never lost); a repeated or idle stop is harmless.
 *  - a bare `go` / `go infinite` stays ACTIVE after its computation finishes and
 *    holds the result until stop / replacement / quit (a bounded condition-variable
 *    wait -- no busy-spin); the FINAL `bestmove` is emitted exactly once, by the
 *    single code path at the end of the worker.
 *  - a replacement `go`, an accepted `position`, `ucinewgame`, `quit` and EOF all
 *    stop + join the old job (completing its output) before replacing the owner or
 *    starting a new one.
 *
 * Output discipline: every protocol line goes through one locked, per-line-flushed
 * emitter (the output mutex is never held across a join or a wait). Diagnostics
 * go to stderr, only when the Debug option is on. `bestmove 0000` is emitted only
 * for a root with no legal move or an internal failure with no known legal move.
 *
 * Implemented UCI: uci, isready, setoption (Debug / Threads=1 / Move Overhead),
 * ucinewgame, position (startpos|fen + a complete move line, transactional),
 * go (depth / nodes / movetime / clock / movestogo / infinite / ponder / mate /
 * searchmoves -- the last three recognized and rejected explicitly), stop, quit.
 * Not implemented (and NOT advertised): MultiPV, Ponder-mode searches, Chess960,
 * Syzygy, Hash (there is no transposition table yet), multiple search workers.
 *
 * The static evaluation is the LABELLED temporary Material eval (search plumbing
 * only); each search announces it once per job with an `info string` line. There is
 * no NNUE and no transposition table yet: advertised `Threads` is fixed at 1.
 */
#ifndef QWC_ENGINE_UCI_HPP
#define QWC_ENGINE_UCI_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/types.hpp"
#include "platform/clock.hpp"
#include "position/position.hpp"
#include "search/search.hpp"

namespace qwc {

class GameState;

/* Parsed `go` arguments. Every field is optional; `valid` is false (with a nonempty
 * `invalid_why`) when the request is malformed or names an unsupported mode -- such
 * a request is rejected and starts NO job. */
struct GoLimits {
  bool has_depth = false, has_nodes = false, has_movetime = false;
  bool has_wtime = false, has_btime = false,
       has_winc = false, has_binc = false, has_movestogo = false;
  bool infinite = false;
  int     depth = 0;
  long long nodes = 0, movetime = 0;
  long long wtime = 0, btime = 0, winc = 0, binc = 0;
  long long movestogo = 0;
  bool valid = true;
  std::string invalid_why;
};

class UciEngine {
 public:
  UciEngine();
  ~UciEngine();
  UciEngine(const UciEngine&) = delete;
  UciEngine& operator=(const UciEngine&) = delete;

  /* The protocol loop (runs on the calling thread): read bounded command lines from
   * `in`, dispatch them, keep ONE search worker alive; on `quit` / EOF stop + join
   * the worker and return the process exit code (0 on a clean stop, 1 on a
   * self-check failure that occurred before the final output). */
  int run(std::istream& in, std::ostream& out);

  /* The engine's protocol identity (the `id` lines and the `--version` output). */
  static constexpr const char* kVersion = "1.0.0";
  static constexpr const char* kName    = "QwenChess";

  /* ---- score presentation at the protocol boundary (independently testable) ---- */

  /* Format a REAL score for an info line: "mate M" for a real mate score (M is the
   * signed UCI move count) or "cp N" otherwise. Returns false (leaving `out` empty)
   * for sentinel values (±INFINITE / ±NONE), which must never appear in a protocol
   * line: the caller omits the score field (and reports the failure, if any). */
  static bool format_score(Value v, std::string& out);

  /* The signed UCI MATE distance (whole moves) of a real mate score: positive when
   * the side to move mates (ply 1 -> 1, ply 3 -> 2, ...), negative when the side to
   * move is mated (ply 2 -> -1, ply 4 -> -2, ...), 0 when mated at the root.
   * Returns 0 for every non-mate value (defensive; format_score gates it). */
  static int mate_moves(Value v);

  /* Parse the `go` arguments (everything after the `go` keyword) into `out`. Returns
   * `out.valid`. Numbers must be nonnegative decimal integers (no sign); a depth is
   * bounded by MAX_PLY; `ponder` / `mate` / `searchmoves` are recognized and
   * rejected explicitly (no unrestricted search may start from them). */
  static bool parse_go(std::span<const std::string_view> args, GoLimits& out);

  /* Resolve parsed limits + the installed root into the search limits for the job.
   * `move_overhead_ms` is the option value. A mixed request with an EXHAUSTED cap
   * (an explicit nodes 0 / movetime 0 / zero usable clock time) sets `zero_budget`
   * and produces a job that returns its legal fallback immediately (a pre-set stop),
   * never an unbounded search. A clock allocation is the documented classical 3x
   * movestogo share of the usable remaining time plus the increment, capped by the
   * usable remaining time. */
  static void resolve_go(const GoLimits& gl, const Position& root, long long move_overhead_ms,
                         SearchLimits& out, bool& zero_budget);

 private:
  struct UciJob;   /* opaque to the public header; defined in uci.cpp */

  void on_line(const std::string& line);
  void cmd_uci();
  void cmd_setoption(const std::vector<std::string_view>& tokens);
  void cmd_position(const std::vector<std::string_view>& tokens);
  void cmd_go(const std::vector<std::string_view>& tokens);
  void request_stop();
  void join_current();
  void worker_main();
  void on_iteration(const SearchResult& res);
  /* The C-style observer trampoline (SearchObserver takes a function pointer + user). */
  static void on_iteration_trampoline(const SearchResult& res, void* user);
  void publish_final(UciJob& job);
  void emit_info_line(const SearchResult& res, const UciJob& job);
  void emit_line(std::string_view line);   /* locked + per-line flushed; stdout only */
  void debug_note(std::string_view note);  /* stderr, only when Debug is on */
  void console_display();
  void console_eval();
  void console_help();

  std::unique_ptr<GameState> game_;
  std::unique_ptr<UciJob> job_;
  std::thread worker_;
  std::mutex out_mutex_;
  std::ostream* out_ = nullptr;
  bool quit_requested_ = false;

  /* Implemented options. */
  bool debug_ = false;
  int threads_ = 1;             /* fixed: exactly one search worker */
  long long move_overhead_ms_ = 0;
};

}  // namespace qwc

#endif /* QWC_ENGINE_UCI_HPP */
