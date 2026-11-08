/* QwenChess engine: the UCI protocol engine (implementation).
 *
 * See uci.hpp for the module contract (responsiveness, cancellation, output
 * discipline, what is and is not implemented). Threading notes (C++ memory
 * model, no volatile / no racy access):
 *  - the ONLY shared mutable state between the reader (main) thread and the worker
 *    thread is: the job's stop flag (std::atomic<bool>), the cancellation
 *    condition variable (its predicate `stop_requested` is guarded by the same
 *    mutex the worker waits on, so a notification is never lost), and the output
 *    stream (a std::mutex around one locked, per-line-flushed write).
 *  - the job's SearchResult is written by the worker and read by the main thread
 *    only AFTER join() (happens-before via the join).
 *  - the worker mutates only its own root Position copy; the installed game state
 *    (whose records carry the repetition history) is read-only for the worker and
 *    outlives it (joined before any replacement).
 */
#include "engine/uci.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <system_error>

#include "core/attacks.hpp"
#include "core/square.hpp"
#include "core/zobrist.hpp"
#include "engine/game_state.hpp"
#include "engine/uci_codec.hpp"
#include "position/fen.hpp"
#include "position/position.hpp"
#include "search/material.hpp"

namespace qwc {

/* ---- the in-flight search job (worker-owned result; reader-driven lifecycle) -- */
struct UciEngine::UciJob {
  Position root{};            /* the mutable root copy; its history borrows the game state */
  SearchLimits limits{};
  TimePoint start{};
  std::atomic<bool> stop{false};   /* cancellation; polled (relaxed) by do_search */
  bool stay_active = false;   /* bare / go infinite: stay active after the computation */
  /* Cancellation handshake: `stop_requested` is guarded by cv_mutex. The setter
   * acquires cv_mutex, sets BOTH the predicate and the atomic, releases, then
   * notifies -- the predicate is transitioned under exactly the mutex that gates the
   * worker's wait, so no notification can be lost between predicate inspection and
   * sleep. */
  std::mutex cv_mutex;
  std::condition_variable cv;
  bool stop_requested = false;
  SearchResult result{};      /* written by the worker; read after join (happens-before) */
};

namespace {

constexpr std::size_t kMaxLineBytes = 128 * 1024;   /* one command line; the bound is documented */

enum class LineResult { Ok, Oversized, Eof };

/* Read at most `kMaxLineBytes` into `line`. An oversized line is DRAINED to its
 * newline (the stream stays in sync) and the whole line is rejected; if EOF is hit
 * while draining, the result is Eof. A trailing partial line at EOF is returned so
 * it is processed, and the NEXT call sees EOF. */
LineResult read_bounded_line(std::istream& in, std::string& line) {
  line.clear();
  int c;
  while ((c = in.get()) != EOF) {
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();   /* tolerate CRLF */
      return LineResult::Ok;
    }
    if (line.size() >= kMaxLineBytes) {
      int d = -1;
      while ((d = in.get()) != EOF && d != '\n') { /* discard the excess */ }
      line.clear();
      return (d == EOF) ? LineResult::Eof : LineResult::Oversized;
    }
    line.push_back((char)c);
  }
  return line.empty() ? LineResult::Eof : LineResult::Ok;
}

void tokenize(const std::string& line, std::vector<std::string_view>& toks) {
  toks.clear();
  const std::string_view sv(line);
  std::size_t i = 0;
  while (i < sv.size()) {
    while (i < sv.size() && (sv[i] == ' ' || sv[i] == '\t')) ++i;
    if (i >= sv.size()) break;
    std::size_t j = i;
    while (j < sv.size() && sv[j] != ' ' && sv[j] != '\t') ++j;
    toks.push_back(sv.substr(i, j - i));
    i = j;
  }
}

/* Case-insensitive equality against a lowercase literal. */
bool ieq(std::string_view a, const char* b) {
  const std::size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (std::size_t i = 0; i < n; ++i)
    if (std::tolower((unsigned char)a[i]) != b[i]) return false;
  return true;
}

/* A strict nonnegative decimal integer: every character an ASCII digit, at least one
 * digit, no sign, no overflow (at most 18 digits, then digit-accumulated against
 * `max_val`). `out` is set only on success. */
bool parse_u64(std::string_view tok, long long& out, long long max_val) {
  if (tok.empty() || tok.size() > 18) return false;
  long long v = 0;
  for (char ch : tok) {
    if (ch < '0' || ch > '9') return false;
    v = v * 10 + (ch - '0');
    if (v > max_val) return false;
  }
  out = v;
  return true;
}

/* Elapsed whole milliseconds since `start` (nonnegative: steady_clock is steady). */
long long elapsed_ms(TimePoint start) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_now() - start).count();
  return ms < 0 ? 0 : (long long)ms;
}

/* The display glyph for a piece (upper = white, lower = black, '.' = empty). */
char piece_char(Piece p) {
  const char c = " PNBRQK"[(int)to_under(p) % 8];
  return to_under(p) >= 8 ? (char)(c | 0x20) : c;   /* black: lowercase */
}

}  // namespace

UciEngine::UciEngine()
  : game_(GameState::build_startpos(std::span<const std::string_view>{})) {
  /* Explicit one-time initialization of the immutable shared tables on the main
   * thread, BEFORE any worker can start: the tables' lazy first-use flags are not
   * synchronization. (Idempotent; main() calls the same inits as well.) */
  attacks_init();
  zobrist_init();
}

UciEngine::~UciEngine() {
  if (worker_.joinable()) {
    request_stop();
    worker_.join();
  }
}

/* ---- public: score presentation / go parsing / go resolution ---------------- */

bool UciEngine::format_score(Value v, std::string& out) {
  out.clear();
  /* Sentinels are never presented: a stopped / failed node's placeholder, and the
   * window infinities. The caller omits the score field and reports the failure. */
  if (v == VALUE_NONE || v == -VALUE_NONE) return false;
  if (v >= VALUE_INFINITE || v <= -VALUE_INFINITE) return false;
  if (is_mate_score(v)) {
    out = "mate " + std::to_string(mate_moves(v));
  } else {
    out = "cp " + std::to_string((int)v);
  }
  return true;
}

int UciEngine::mate_moves(Value v) {
  if (!is_mate_score(v)) return 0;
  /* The score's ply distance (root-relative; a real mate score has 0..MAX_PLY). */
  const int abs_ply = v > 0 ? (int)(VALUE_MATE - v) : (int)(v + VALUE_MATE);
  if (abs_ply <= 0) return 0;               /* mated at the root */
  const int moves = (abs_ply + 1) / 2;      /* ply 1 -> 1 move; ply 3 -> 2; ply 2 -> 1; ... */
  return v > 0 ? moves : -moves;
}

bool UciEngine::parse_go(std::span<const std::string_view> args, GoLimits& out) {
  out = GoLimits{};
  const long long kBig = std::numeric_limits<long long>::max();
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view t = args[i];
    long long v = 0;
    if (ieq(t, "depth")) {
      if (i + 1 >= args.size() || !parse_u64(args[++i], v, MAX_PLY)) {
        out.valid = false; out.invalid_why = "rejected: `depth` needs a value 1.." + std::to_string(MAX_PLY);
        return false;
      }
      if (v < 1) {
        out.valid = false; out.invalid_why = "rejected: `depth` must be >= 1";
        return false;
      }
      out.depth = (int)v; out.has_depth = true;
    } else if (ieq(t, "nodes")) {
      if (i + 1 >= args.size() || !parse_u64(args[++i], v, kBig)) {
        out.valid = false; out.invalid_why = "rejected: `nodes` needs a nonnegative integer";
        return false;
      }
      out.nodes = v; out.has_nodes = true;
    } else if (ieq(t, "movetime")) {
      if (i + 1 >= args.size() || !parse_u64(args[++i], v, kBig)) {
        out.valid = false; out.invalid_why = "rejected: `movetime` needs a nonnegative integer (ms)";
        return false;
      }
      out.movetime = v; out.has_movetime = true;
    } else if (ieq(t, "wtime")) {
      if (i + 1 >= args.size() || !parse_u64(args[++i], v, kBig)) {
        out.valid = false; out.invalid_why = "rejected: `wtime` needs a nonnegative integer (ms)";
        return false;
      }
      out.wtime = v; out.has_wtime = true;
    } else if (ieq(t, "btime")) {
      if (i + 1 >= args.size() || !parse_u64(args[++i], v, kBig)) {
        out.valid = false; out.invalid_why = "rejected: `btime` needs a nonnegative integer (ms)";
        return false;
      }
      out.btime = v; out.has_btime = true;
    } else if (ieq(t, "winc")) {
      if (i + 1 >= args.size() || !parse_u64(args[++i], v, kBig)) {
        out.valid = false; out.invalid_why = "rejected: `winc` needs a nonnegative integer (ms)";
        return false;
      }
      out.winc = v; out.has_winc = true;
    } else if (ieq(t, "binc")) {
      if (i + 1 >= args.size() || !parse_u64(args[++i], v, kBig)) {
        out.valid = false; out.invalid_why = "rejected: `binc` needs a nonnegative integer (ms)";
        return false;
      }
      out.binc = v; out.has_binc = true;
    } else if (ieq(t, "movestogo")) {
      if (i + 1 >= args.size() || !parse_u64(args[++i], v, kBig)) {
        out.valid = false; out.invalid_why = "rejected: `movestogo` needs an integer";
        return false;
      }
      if (v < 1) {
        out.valid = false; out.invalid_why = "rejected: `movestogo` must be >= 1";
        return false;
      }
      out.movestogo = v; out.has_movestogo = true;
    } else if (ieq(t, "infinite")) {
      out.infinite = true;
    } else if (ieq(t, "ponder")) {
      out.valid = false; out.invalid_why = "rejected: `go ponder` is not supported";
      return false;
    } else if (ieq(t, "mate")) {
      out.valid = false; out.invalid_why = "rejected: `go mate` is not supported";
      return false;
    } else if (ieq(t, "searchmoves")) {
      out.valid = false; out.invalid_why = "rejected: `go searchmoves` is not supported";
      return false;
    } else {
      out.valid = false;
      out.invalid_why = "rejected: unknown go limit `" + std::string(t) + "`";
      return false;
    }
  }
  return true;
}

void UciEngine::resolve_go(const GoLimits& gl, const Position& root, long long move_overhead_ms,
                           SearchLimits& out, bool& zero_budget) {
  out = SearchLimits{};
  zero_budget = false;
  if (gl.has_depth) out.depth = gl.depth;
  if (gl.has_nodes) out.max_nodes = gl.nodes;

  long long deadline_ms = -1;   /* -1 = no deadline yet */
  if (gl.has_movetime) deadline_ms = gl.movetime;

  /* Clock allocation (documented classical heuristic): spend 3x the per-move share
   * of the usable remaining time, plus the increment, capped by the usable remaining
   * time. The side-to-move's own time/increment fields are used; a requested clock
   * whose side field is absent, or zero usable time after the move overhead, is an
   * exhausted cap (an immediate legal fallback), never an unbounded search. */
  const bool has_clock = gl.has_wtime || gl.has_btime;
  long long usable = -1;   /* -1 = no clock component */
  if (has_clock) {
    const bool white = root.side == Color::WHITE;
    const long long rem = white ? gl.wtime : gl.btime;   /* 0 when the field was absent */
    const long long inc = white ? gl.winc : gl.binc;
    const long long m2g = gl.has_movestogo ? gl.movestogo : 30;   /* the documented default */
    usable = rem - move_overhead_ms;
    if (usable < 0) usable = 0;
    long long spend = 0;
    if (usable > 0) {
      spend = usable / m2g * 3 + inc;
      if (spend > usable) spend = usable;
    }
    deadline_ms = (deadline_ms < 0) ? spend : ((spend < deadline_ms) ? spend : deadline_ms);
  }

  /* A bare `go` means `go infinite` per the UCI specification. */
  const bool bare = !gl.has_depth && !gl.has_nodes && !gl.has_movetime && !gl.has_wtime
                    && !gl.has_btime && !gl.has_movestogo && !gl.infinite;
  if (gl.infinite || bare) out.infinite = true;

  /* An EXPLICIT exhausted cap wins over everything: the job is pre-stopped and
   * returns its legal fallback (a real depth-0 result), never an unbounded search. */
  if ((gl.has_nodes && gl.nodes == 0) || (gl.has_movetime && gl.movetime == 0)
      || (usable >= 0 && usable <= 0)) {
    zero_budget = true;
    out = SearchLimits{};   /* no internal caps; the pre-set stop terminates the root */
  }

  if (!zero_budget && deadline_ms >= 0) {
    out.has_deadline = true;
    out.deadline = clock_now() + std::chrono::milliseconds(deadline_ms);
  }
}

/* ---- the protocol loop ------------------------------------------------------- */

int UciEngine::run(std::istream& in, std::ostream& out) {
  out_ = &out;
  if (!game_) game_ = GameState::build_startpos(std::span<const std::string_view>{});

  for (;;) {
    std::string line;
    const LineResult rc = read_bounded_line(in, line);
    if (rc == LineResult::Eof) {
      request_stop();   /* EOF stops the engine cleanly */
      break;
    }
    if (rc == LineResult::Oversized) {
      debug_note("oversized command line (>" + std::to_string(kMaxLineBytes) + " bytes) rejected");
      continue;         /* the stream is back in sync; the next command is parsed normally */
    }
    on_line(line);
    if (quit_requested_) {
      request_stop();
      break;
    }
  }
  join_current();   /* the worker has emitted its final output before this returns */
  return 0;
}

void UciEngine::on_line(const std::string& line) {
  std::vector<std::string_view> toks;
  tokenize(line, toks);
  if (toks.empty()) return;

  const std::string_view cmd = toks[0];
  if (ieq(cmd, "uci")) {
    cmd_uci();
  } else if (ieq(cmd, "isready")) {
    /* Answer even while a job is active: this does NOT stop or join it. */
    emit_line("readyok");
  } else if (ieq(cmd, "debug")) {
    if (toks.size() > 1) debug_ = ieq(toks[1], "on");
  } else if (ieq(cmd, "setoption")) {
    cmd_setoption(toks);
  } else if (ieq(cmd, "ucinewgame")) {
    request_stop();
    join_current();
    game_ = GameState::build_startpos(std::span<const std::string_view>{});
  } else if (ieq(cmd, "position")) {
    cmd_position(toks);
  } else if (ieq(cmd, "go")) {
    cmd_go(toks);
  } else if (ieq(cmd, "stop")) {
    request_stop();   /* a repeated or idle stop is harmless */
  } else if (ieq(cmd, "quit")) {
    quit_requested_ = true;   /* run() performs the stop + join + clean exit */
  } else if (ieq(cmd, "ponderhit")) {
    /* recognized and ignored: ponder is not a supported search mode */
  } else if (ieq(cmd, "display") || ieq(cmd, "show") || ieq(cmd, "d")) {
    console_display();
  } else if (ieq(cmd, "eval")) {
    console_eval();
  } else if (ieq(cmd, "help")) {
    console_help();
  } else {
    debug_note("unknown command `" + std::string(cmd) + "` (ignored)");
  }
}

void UciEngine::cmd_uci() {
  emit_line(std::string("id name ") + kName + " " + kVersion);
  emit_line(std::string("id author ") + kName + " developers");
  /* Only the implemented options are advertised. */
  emit_line("option name Debug type check default off");
  emit_line("option name Threads type spin min 1 max 1 default 1");
  emit_line("option name Move Overhead type spin min 0 max 10000 default 0");
  emit_line("uciok");
}

void UciEngine::cmd_setoption(const std::vector<std::string_view>& t) {
  if (t.size() < 2 || !ieq(t[1], "name")) {
    debug_note("setoption: malformed (expected `name ... value ...`)");
    return;
  }
  std::size_t i = 2;
  std::string name;
  for (; i < t.size() && !ieq(t[i], "value"); ++i) {
    if (!name.empty()) name += ' ';
    name += t[i];
  }
  if (i >= t.size()) {
    debug_note("setoption: missing `value`");
    return;
  }
  std::string value;
  for (++i; i < t.size(); ++i) {
    if (!value.empty()) value += ' ';
    value += t[i];
  }
  std::string lname = name;
  for (char& c : lname) c = (char)std::tolower((unsigned char)c);

  long long v = 0;
  if (lname == "debug") {
    if (ieq(value, "on")) debug_ = true;
    else if (ieq(value, "off")) debug_ = false;
    else debug_note("setoption: Debug expects on|off");
  } else if (lname == "threads") {
    /* Exactly one search worker: the option is accepted only at its single value. */
    if (parse_u64(value, v, 1000000) && v >= 1 && v <= 1) threads_ = (int)v;
    else debug_note("setoption: Threads is fixed at 1 (single search worker)");
  } else if (lname == "move overhead") {
    if (parse_u64(value, v, 10000)) move_overhead_ms_ = v;
    else debug_note("setoption: `Move Overhead` must be 0..10000 ms");
  } else {
    debug_note("setoption: unknown option `" + name + "`");
  }
}

void UciEngine::cmd_position(const std::vector<std::string_view>& t) {
  bool startpos = false;
  std::string fen;
  std::span<const std::string_view> moves;

  if (t.size() < 2) {
    debug_note("position: missing `startpos` or `fen` (state unchanged)");
    return;
  }
  if (ieq(t[1], "startpos")) {
    startpos = true;
    /* The spec form is `position startpos move1 move2 ...`; the `moves` keyword is
     * legal only after a FEN but is sent after `startpos` by common clients as well,
     * so it is tolerated (and skipped) in both forms. */
    std::size_t i = 2;
    if (i < t.size() && ieq(t[i], "moves"))
      ++i;
    moves = std::span<const std::string_view>(t.data() + i, t.size() - i);
  } else if (ieq(t[1], "fen")) {
    if (t.size() < 8) {
      debug_note("position: a FEN needs exactly six fields (state unchanged)");
      return;
    }
    for (int i = 0; i < 6; ++i) {
      if (i) fen += ' ';
      fen += t[2 + i];
    }
    std::size_t idx = 8;
    if (idx < t.size()) {
      if (ieq(t[idx], "moves")) ++idx;
      else {
        debug_note("position: expected the `moves` keyword after the six FEN fields (state unchanged)");
        return;
      }
    }
    moves = std::span<const std::string_view>(t.data() + idx, t.size() - idx);
  } else {
    debug_note("position: expected `startpos` or `fen` (state unchanged)");
    return;
  }

  /* Transactional: the complete new state is built first; any failure keeps the
   * previously installed state (and any active job on it) exactly as it is. */
  std::string_view err;
  /* NOT const: the owning pointer is transferred into `game_` on full success. */
  auto built = startpos ? GameState::build_startpos(moves)
                        : GameState::build_fen(fen, moves, &err);
  if (!built) {
    debug_note(std::string("position rejected: ") + std::string(err) + " (previous state kept)");
    return;
  }
  request_stop();
  join_current();
  game_ = std::move(built);
}

void UciEngine::cmd_go(const std::vector<std::string_view>& t) {
  GoLimits gl;
  if (!parse_go(std::span<const std::string_view>(t.data() + 1, t.size() > 1 ? t.size() - 1 : 0), gl)) {
    /* A clear, explicit rejection on the protocol stream; NO job is started. */
    emit_line("info string " + gl.invalid_why);
    return;
  }
  /* A replacement go completes the old job's output before starting the new one. */
  request_stop();
  join_current();

  auto job = std::make_unique<UciJob>();
  job->root = game_->pos();   /* owns a mutable root copy; borrows the game's history */
  bool zero_budget = false;
  resolve_go(gl, job->root, move_overhead_ms_, job->limits, zero_budget);
  job->stay_active = job->limits.infinite && !zero_budget;
  job->start = clock_now();
  if (zero_budget) job->stop.store(true);   /* pre-set stop: the root entry yields a fallback */

  job_ = std::move(job);
  try {
    worker_ = std::thread(&UciEngine::worker_main, this);
  } catch (const std::system_error&) {
    /* Thread creation failed (resource exhaustion): abandon the job rather than
     * leave a job that will never finish. The engine stays responsive; the GUI
     * receives no bestmove for this go and will re-issue it. */
    job_.reset();
    debug_note("could not start the search worker thread (resource exhaustion)");
  }
}

/* ---- cancellation / lifecycle ------------------------------------------------ */

void UciEngine::request_stop() {
  if (!job_) return;   /* idle: a repeated or idle stop is a no-op */
  {
    std::lock_guard lk(job_->cv_mutex);   /* coordinate the predicate transition */
    job_->stop_requested = true;
    job_->stop.store(true, std::memory_order_release);
  }
  job_->cv.notify_all();   /* after releasing the mutex: a sleeping worker wakes, rechecks */
}

void UciEngine::join_current() {
  if (worker_.joinable()) {
    worker_.join();   /* the worker has fully finished -- including its final output */
  }
  job_.reset();
}

void UciEngine::worker_main() {
  UciJob& job = *job_;
  /* Announce the temporary evaluation once per search (it is never NNUE). */
  emit_line("info string evaluation: temporary Material (search plumbing only; not NNUE)");

  SearchObserver obs{&UciEngine::on_iteration_trampoline, this};
  job.result = do_search(job.root, job.limits, job.stop, obs);

  if (job.stay_active) {
    /* A bare / infinite job stays ACTIVE after the computation: a bounded
     * condition-variable wait (no busy-spin) until stop / replacement / quit. */
    std::unique_lock lk(job.cv_mutex);
    job.cv.wait(lk, [&job] { return job.stop_requested; });
  }

  publish_final(job);   /* the one code path that emits the final bestmove */
}

void UciEngine::on_iteration(const SearchResult& res) {
  if (job_) emit_info_line(res, *job_);
}

void UciEngine::on_iteration_trampoline(const SearchResult& res, void* user) {
  static_cast<UciEngine*>(user)->on_iteration(res);
}

/* ---- output ------------------------------------------------------------------ */

void UciEngine::emit_line(std::string_view line) {
  std::lock_guard lk(out_mutex_);   /* one locked, per-line-flushed write: serialized */
  *out_ << line << '\n';
  out_->flush();
}

void UciEngine::debug_note(std::string_view note) {
  if (!debug_) return;   /* diagnostics: stderr only, and only when Debug is on */
  std::cerr << "qwenchess: " << note << '\n';
}

void UciEngine::emit_info_line(const SearchResult& res, const UciJob& job) {
  std::string line = "info depth " + std::to_string(res.completed_depth);
  std::string score;
  /* The UCI protocol requires the `score` keyword before the score field
   * (`info ... score cp 5 ...` / `score mate 2 ...`); GUIs key their parsers on it. */
  if (format_score(res.score, score)) line += " score " + score;   /* the score field, if real */
  line += " nodes " + std::to_string(res.nodes);
  const long long ms = elapsed_ms(job.start);
  line += " time " + std::to_string(ms);
  if (ms > 0 && res.nodes > 0) line += " nps " + std::to_string(res.nodes * 1000 / ms);
  if (res.pv_size > 0) {
    for (int i = 0; i < res.pv_size; ++i) {
      char tok[kUciMoveText];
      if (uci_move_emit(res.pv[i], tok, sizeof tok) > 0) {
        line += " ";
        line += tok;
      }
    }
  } else if (res.bestmove != move_none()) {
    /* A depth-0 (fallback) result presents the known legal move as a one-move PV. */
    char tok[kUciMoveText];
    if (uci_move_emit(res.bestmove, tok, sizeof tok) > 0) {
      line += " ";
      line += tok;
    }
  }
  emit_line(line);
}

void UciEngine::publish_final(UciJob& job) {
  SearchResult& res = job.result;
  if (res.failed) {
    /* An internal failure is reported explicitly (never as a normal score), and
     * 0000 is written only when no legal move is known (a root with no legal move
     * is the one other honest 0000). */
    emit_line("info string internal search failure (move generation or application error)");
  } else {
    emit_info_line(res, job);   /* even for a terminal / immediate-fallback job */
  }
  char tok[kUciMoveText];
  std::string best = "0000";
  if (uci_move_emit(res.bestmove, tok, sizeof tok) > 0) best = tok;   /* 0000 for move_none */
  emit_line("bestmove " + best);
}

/* ---- console helpers (read-only over the installed game state) ---------------- */

void UciEngine::console_display() {
  const Position& pos = game_->pos();
  emit_line("info string   +----------------+");
  for (int r = 7; r >= 0; --r) {
    std::string s = "info string " + std::to_string(r + 1) + " |";
    for (int f = 0; f < 8; ++f) {
      s += ' ';
      s += piece_char(pos.mailbox[square_of((Square)f, (Square)r)]);
    }
    s += " |";
    emit_line(s);
  }
  emit_line("info string   +----------------+");
  emit_line("info string     a  b  c  d  e  f  g  h");
  emit_line(std::string("info string side: ") + (pos.side == Color::WHITE ? "white" : "black"));
  char fbuf[128];
  if (fen_emit(pos, std::span<char>(fbuf, sizeof fbuf)))
    emit_line(std::string("info string fen: ") + fbuf);
  emit_line("info string halfmove: " + std::to_string(pos.halfmove)
            + "  fullmove: " + std::to_string(pos.fullmove));
}

void UciEngine::console_eval() {
  /* The LABELLED temporary material evaluation (side-to-move perspective), in cp. */
  const Value v = eval_material(game_->pos());
  const int a = v < 0 ? -v : (int)v;
  emit_line(std::string("info string eval (temporary material, side to move; not NNUE): ")
            + (v < 0 ? "-" : "+") + std::to_string(a) + " cp");
}

void UciEngine::console_help() {
  emit_line(std::string("info string ") + kName + " commands: uci, isready, setoption, ucinewgame, position, go, stop, quit, display, eval, help");
  emit_line("info string setoption: `Debug` (on|off) | `Threads` (fixed 1) | `Move Overhead` (0..10000 ms)");
  emit_line("info string go: [depth N] [nodes N] [movetime MS] [wtime MS btime MS] [winc MS binc MS] [movestogo N] [infinite]");
  emit_line("info string not implemented (not advertised): MultiPV, ponder, mate, searchmoves, Hash, Syzygy, Chess960, multiple workers");
  emit_line("info string evaluation: temporary Material (search plumbing only) until the NNUE milestone");
  emit_line("info string transposition table: not implemented yet");
}

}  // namespace qwc
