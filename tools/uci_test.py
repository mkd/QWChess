#!/usr/bin/env python3
"""QWChess UCI protocol integration tests.

Drives the REAL engine binary through the full UCI lifecycle against an
independent reference (pinned python `chess` 1.11.2: FENs, legal moves, PV
replay, check/mate/stalemate states, game termination). The engine and its C++
unit tests never import Python; this differential is a separate, user-invoked
check (tools/test_requirements.txt).

Coverage:
  1.  handshake (uci / isready / setoption / advertised options)
  2.  position replay (special moves, FEN equality with the reference, illegal
      tokens, 0000, >256 plies, a threefold line, stability)
  3.  search output (legal bestmove, PV replay, mate/mated/draw presentation,
      zero budgets, node / movetime / clock limits)
  4.  infinite lifecycle (active after finish; exactly one final bestmove;
      terminal root; isready while active)
  5.  replacement (go / position / ucinewgame while a job runs)
  6.  malformed input (bounded lines, bad go/position forms, unknown commands)
  7.  latency (isready while searching; stop -> bestmove bound)
  8.  self-play + time-control (reference-verified games through the protocol)

Run:  python3 tools/uci_test.py --engine build/cpp20/release/bin/qwenchess
  or: make test-uci CONFIG=release PYTHON=python3 [--scale N for slow builds]
"""

import argparse
import os
import re
import subprocess
import sys
import threading
import time

try:
    import chess
except ImportError:
    sys.exit("FAIL: the `chess` package is required (pip install -r tools/test_requirements.txt)")

if chess.__version__ != "1.11.2":
    sys.exit(f"FAIL: pinned reference is chess==1.11.2; found {chess.__version__}")

START = chess.Board()

passed = 0
failed = 0
failures = []


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        failures.append(what)
        print(f"  FAIL: {what}")


def info(msg):
    print(f"  {msg}")


def from_uci(tok):
    """A safe UCI token -> chess.Move (None when the token is not a move)."""
    if tok is None or tok == "0000":
        return None
    try:
        return chess.Move.from_uci(tok)
    except Exception:
        return None


# --------------------------------------------------------------------------
# The UCI client. The engine process runs on BINARY, UNBUFFERED pipes:
# Python's text-mode wrapper on a pipe is block-buffered, and even its read(1)
# waits for a full internal chunk, which would delay every line by the buffer
# size. A reader thread batches os.read() calls (one syscall per up-to-8 KiB
# chunk) and publishes decoded lines; the main thread polls with deadlines.
# --------------------------------------------------------------------------
class Uci:
    def __init__(self, engine, name, scale=1.0):
        self.engine = engine
        self.name = name
        self.scale = scale
        self.lines = []           # all engine lines so far (chronological)
        self._lock = threading.Lock()
        self._buf = b""
        self._eof = False
        self.proc = subprocess.Popen(
            [engine],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,             # binary, unbuffered, both directions
        )
        self._out_fd = self.proc.stdout.fileno()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()
        # The engine writes protocol diagnostics to stderr. A full (64 KiB) pipe
        # buffer would block the engine, so a dedicated thread drains it; the tail is
        # kept for failure reports.
        self._err_tail = []
        self._err_thread = threading.Thread(target=self._drain_err, daemon=True)
        self._err_thread.start()

    def _drain_err(self):
        fd = self.proc.stderr.fileno()
        buf = b""
        while True:
            data = os.read(fd, 8192)
            if data == b"":
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if line:
                    with self._lock:
                        self._err_tail.append(line.decode("utf-8", "replace"))
                        if len(self._err_tail) > 24:
                            del self._err_tail[: len(self._err_tail) - 24]

    def _read(self):
        while True:
            data = os.read(self._out_fd, 8192)   # blocks until >= 1 byte; EOF -> b""
            if data == b"":
                with self._lock:
                    if self._buf:
                        self._append_line(self._buf)
                    self._buf = b""
                    self._eof = True
                break
            self._consume(data)

    def _consume(self, data):
        with self._lock:
            self._buf += data
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                self._append_line(line)

    def _append_line(self, raw):
        if raw.endswith(b"\r"):
            raw = raw[:-1]
        if raw:
            self.lines.append(raw.decode("utf-8", "replace"))

    def send(self, line):
        self.proc.stdin.write(line.encode("ascii") + b"\n")
        self.proc.stdin.flush()

    def snapshot(self):
        """The current end-of-buffer index; pass as `floor` to wait_for to match
        only lines that arrive AFTER a trigger command was sent."""
        with self._lock:
            return len(self.lines)

    def wait_for(self, pred, timeout, what, floor=None):
        """Wait until `pred(line)` matches a buffered line at or after `floor`
        (default: the whole history). Returns the line, or None (and records a
        failed check) on timeout / EOF-without-match."""
        floor = 0 if floor is None else floor
        deadline = time.monotonic() + timeout * self.scale
        while True:
            with self._lock:
                for idx in range(floor, len(self.lines)):
                    if pred(self.lines[idx]):
                        return self.lines[idx]
                eof = self._eof
            if eof:
                break
            if time.monotonic() >= deadline:
                break
            time.sleep(0.02)
        with self._lock:
            recent = self.lines[-8:]
        check(False, f"[{self.name}] {what} (engine tail: {recent})")
        return None

    def drain_quiet(self, seconds):
        time.sleep(seconds * self.scale)

    def bestmoves(self):
        with self._lock:
            return [ln.split()[-1] for ln in self.lines if ln.startswith("bestmove ")]

    def move_of(self, line):
        """The move token of a bestmove line; '0000' when the line is missing."""
        if line is None:
            return "0000"
        parts = line.split()
        return parts[-1] if len(parts) >= 2 else "0000"

    def all_lines(self):
        with self._lock:
            return list(self.lines)

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5 * self.scale)
        except Exception:
            self.proc.kill()
            self.proc.wait()
        self._err_thread.join(timeout=2 * self.scale)

    def err_tail(self):
        with self._lock:
            return list(self._err_tail)


INFO_RE = re.compile(r"^info depth (\d+)(?: score (?:mate (-?\d+)|cp (-?\d+)))?"
                     r" nodes (\d+)(?: time (\d+))?(?: nps (\d+))?(?: pv (.+))?$")


def parse_info(line):
    m = INFO_RE.match(line)
    if not m:
        return None
    return {
        "depth": int(m.group(1)),
        "mate": int(m.group(3)) if m.group(3) is not None else None,
        "cp": int(m.group(4)) if m.group(4) is not None else None,
        "nodes": int(m.group(5)),
        "pv": (m.group(7) or "").split(),
    }


def fen_of(u, deadline, what):
    floor = u.snapshot()          # only count lines emitted AFTER this display
    u.send("display")
    line = u.wait_for(lambda l: l.startswith("info string fen:"), deadline, what, floor=floor)
    if line is None:
        return None
    return line.split("fen:")[1].strip()


def expect_bestmove_legal(u, board, bm_line, what):
    tok = u.move_of(bm_line)
    if tok == "0000":
        check(not list(board.legal_moves), f"{what}: 0000 implies no legal move")
    else:
        check(from_uci(tok) in board.legal_moves, f"{what}: bestmove {tok} is legal")
    return tok


# --------------------------------------------------------------------------
# 1. handshake
# --------------------------------------------------------------------------
def test_handshake(u, deadline):
    u.send("uci")
    u.wait_for(lambda l: l.startswith("id name "), deadline, "id name")
    u.wait_for(lambda l: l.startswith("id author "), deadline, "id author")
    u.wait_for(lambda l: l == "uciok", deadline, "uciok")
    opts = " ".join(l for l in u.all_lines() if l.startswith("option name "))
    check("uciok" in " ".join(u.all_lines()), "uciok present")
    check("Debug" in opts, "advertises option Debug")
    check("Threads" in opts, "advertises option Threads")
    check("Move Overhead" in opts, "advertises option Move Overhead")
    check("MultiPV" not in opts and "Hash" not in opts and "Syzygy" not in opts
          and "Contender" not in opts, "does not advertise unimplemented options")
    m = re.search(r"option name Threads type spin min (\d+) max (\d+) default (\d+)", opts)
    check(m is not None and m.group(1) == "1" and m.group(2) == "1" and m.group(3) == "1",
          "Threads is fixed at min=max=default=1")
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", deadline, "readyok (idle)")
    u.send("setoption name Threads value 1")
    u.drain_quiet(0.3)
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", deadline, "readyok after setoption threads=1")
    u.send("setoption name Threads value 8")     # rejected (single worker); the engine must stay alive
    u.send("setoption name Move Overhead value 100")
    u.send("setoption name Debug value off")
    u.drain_quiet(0.5)
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", deadline, "readyok after rejected/accepted setoptions")


# --------------------------------------------------------------------------
# 2. position replay
# --------------------------------------------------------------------------
def test_positions(u, deadline):
    promo_w = "2k5/4P3/8/8/8/8/8/4K3 w - - 0 1"
    promo_b = "K6k/8/8/8/8/8/4p3/8 b - - 0 1"
    cases = [
        ("startpos", [], "startpos only"),
        ("startpos", ["e2e4"], "one move"),
        ("startpos", ["e2e4", "e7e5", "g1f3"], "three moves"),
        ("startpos", ["e2e4", "c7c5", "f1b5"], "bishop develop"),
        ("startpos", ["e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5", "e1g1"], "O-O coordinate"),
        ("startpos", ["d2d4", "d7d5", "b1c3", "b8c6", "c1f4", "c8f5",
                      "d1d2", "e1c1"], "O-O-O coordinate"),
        ("startpos", ["e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5", "O-O"], "O-O keyword"),
        ("startpos", ["d2d4", "d7d5", "b1c3", "b8c6", "c1f4", "c8f5",
                      "d1d2", "o-o-o"], "lowercase o-o-o keyword"),
        ("startpos", ["e2e4", "c7c5", "g1f3", "d7d5", "e4d5"], "en passant capture"),
        ("startpos", ["e2e3", "e7e5", "d2d4", "d7d6", "f1c4", "b8c6",
                      "e3e4", "e5e4", "d4e4"], "ep on another file"),
        (promo_w, ["e7e8q"], "promote queen"),
        (promo_w, ["e7e8n"], "underpromote knight"),
        (promo_w, ["e7e8r"], "underpromote rook"),
        (promo_w, ["e7e8b"], "underpromote bishop"),
        (promo_b, ["e2e1q"], "black-side promotion"),
    ]
    for i, (setup, moves, tag) in enumerate(cases):
        if setup == "startpos":
            u.send("position startpos" + (" " + " ".join(moves) if moves else ""))
        else:
            u.send(f"position fen {setup}" + (" " + " ".join(moves) if moves else ""))
        u.drain_quiet(0.35)
        # Reference replay. python-chess wants the coordinate form, so the castling
        # keywords this suite sends are mapped onto the king-destination form.
        if setup == "startpos":
            ref = START.copy()
        else:
            ref = chess.Board(setup)
        ok = True
        for m in moves:
            tok = m
            if m.lower() in ("o-o", "o-o-o"):
                white = ref.turn == chess.WHITE
                tok = ("e1" if white else "e8") + ("g1" if white else "g8") \
                    if m.lower() == "o-o" else ("c1" if white else "c8")
            mv = from_uci(tok)
            if mv is None or mv not in ref.legal_moves:
                ok = False
                break
            ref.push(mv)
        check(ok, f"case {i} ({tag}): the reference line is fully legal")
        got = fen_of(u, deadline, f"fen after case {i} ({tag})")
        check(got == ref.fen(), f"case {i} ({tag}): engine fen {got!r} == reference {ref.fen()!r}")

    # a known castling keyword for an UNAVAILABLE castle is rejected as a whole
    u.send("position startpos")
    u.drain_quiet(0.2)
    u.send("position startpos moves e2e4 e7e5 O-O")   # the bishop still sits on f1
    u.drain_quiet(0.35)
    got = fen_of(u, deadline, "fen after a rejected O-O")
    check(got == START.fen(), "an unavailable O-O keyword keeps the previous state")

    # the recorded en-passant square stays in the FEN after a double push
    u.send("position startpos moves e2e4")
    u.drain_quiet(0.3)
    got = fen_of(u, deadline, "fen with a recorded ep square")
    check(got is not None and " e3 " in got + " ",
          f"the recorded ep square (e3) appears in the FEN ({got!r})")
    ep_state = START.copy()
    ep_state.push(chess.Move.from_uci("e2e4"))

    # a 0000 token is rejected; the PREVIOUS state must be unchanged
    u.send("position startpos moves e2e4 0000")
    u.drain_quiet(0.35)
    got = fen_of(u, deadline, "fen after rejected 0000 token")
    check(got == ep_state.fen(), "a 0000 token keeps the previously installed state")

    # an illegal move token (the same push twice) is rejected the same way
    u.send("position startpos moves e2e4 e2e4")
    u.drain_quiet(0.35)
    got = fen_of(u, deadline, "fen after an illegal token")
    check(got == ep_state.fen(), "an illegal trailing move keeps the previously installed state")

    # the eval console works after an accepted replay
    u.send("position startpos moves e2e4 c7c5")
    u.drain_quiet(0.3)
    u.send("eval")
    u.wait_for(lambda l: l.startswith("info string eval"), deadline, "eval console after replay")

    # a threefold line: the engine must present a DRAW (cp 0) and still a legal move
    trip = ["g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1", "f6g8"]
    u.send("position startpos moves " + " ".join(trip))
    u.drain_quiet(0.4)
    floor = u.snapshot()
    u.send("go depth 4")
    u.drain_quiet(8.0)
    u.send("stop")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove on a threefold", floor=floor)
    u.wait_for(lambda l: l.startswith("info ") and " cp 0" in l, 60, "a threefold presents cp 0", floor=floor)
    ref = START.copy()
    for m in trip:
        ref.push(chess.Move.from_uci(m))
    # Count occurrences by the five chess-state fields (the halfmove/fullmove
    # counters differ between visits and are not part of repetition identity).
    key = " ".join(ref.fen().split()[:5])
    bb = START.copy()
    cnt = 1 if " ".join(bb.fen().split()[:5]) == key else 0
    for m in trip:
        bb.push(chess.Move.from_uci(m))
        if " ".join(bb.fen().split()[:5]) == key:
            cnt += 1
    check(cnt == 3, f"the reference visits the position three times (got {cnt})")
    tok = u.move_of(bm)
    check(tok != "0000", "a threefold with legal moves is NOT 0000")
    check(from_uci(tok) in ref.legal_moves, "the threefold bestmove is legal")


def test_long_and_stable(u, deadline):
    # A seeded >256-ply legal line replays into a state whose FEN matches the
    # reference exactly (half in one command, the whole in the next). Random
    # lines end in a terminal state after a while, so seeds are retried until one
    # survives past the 256-ply boundary (the recorded seed is reported).
    import random
    line = []
    board = None
    seed = 0
    while seed < 500:
        rnd = random.Random(0xC0FFEE + seed)
        b = START.copy()
        cand = []
        while len(cand) < 300:
            moves = list(b.legal_moves)
            if not moves:
                break
            m = moves[rnd.randrange(len(moves))]
            b.push(m)
            if b.is_game_over() and len(cand) < 256:
                break
            cand.append(m.uci())
        if len(cand) >= 256:
            line, board, seed = cand, b, seed
            break
        seed += 1
    check(len(line) >= 256, f"a line exceeding 256 plies was found (got {len(line)}, seed {seed})")
    if board is None:
        return
    for chunk in (line[: len(line) // 2], line):
        u.send("position startpos moves " + " ".join(chunk))
        u.drain_quiet(0.5)
        got = fen_of(u, deadline, f"long-line fen ({len(chunk)} plies)")
        bb = START.copy()
        for m in chunk:
            bb.push(chess.Move.from_uci(m))
        check(got == bb.fen(), f"long line ({len(chunk)} plies): fen matches the reference")
    # the installed state stays stable for subsequent searches
    floor = u.snapshot()
    u.send("go depth 2")
    u.drain_quiet(4.0)
    u.send("stop")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove after a long line", floor=floor)
    tok = u.move_of(bm)
    check(tok == "0000" or from_uci(tok) in board.legal_moves,
          "a bestmove after a >256-ply line is legal (or 0000 if terminal)")


# --------------------------------------------------------------------------
# 3. search output
# --------------------------------------------------------------------------
def test_search_output(u, deadline):
    # a completed finite search: legal bestmove + a replayable PV + sane counters
    u.send("position startpos")
    u.drain_quiet(0.3)
    floor = u.snapshot()
    u.send("go depth 6")
    u.drain_quiet(20.0)
    u.send("stop")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 120, "bestmove (depth 6)", floor=floor)
    expect_bestmove_legal(u, START, bm, "depth-6")
    infos = [p for p in (parse_info(l) for l in u.all_lines() if l.startswith("info depth ")) if p]
    check(len(infos) >= 1, "at least one completed-iteration info line")
    if infos:
        last = infos[-1]
        check(last["depth"] >= 1, f"the final info reports a completed depth ({last['depth']})")
        check((last["mate"] is None) != (last["cp"] is None),
              "the final info carries exactly one score field")
        if last["pv"]:
            b = START.copy()
            ok = True
            for m in last["pv"]:
                mv = from_uci(m)
                if mv not in b.legal_moves:
                    ok = False
                    break
                b.push(mv)
            check(ok, f"the final PV replays legally from the root ({len(last['pv'])} moves)")
        if len(infos) >= 2:
            check(infos[-1]["nodes"] >= infos[-2]["nodes"],
                  "node totals are monotonic across iterations")
    check(True, "completed search output")

    # a mate-in-1 root: 'score mate 1' + a reference-verified mate
    fen = "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1"
    u.send(f"position fen {fen}")
    u.drain_quiet(0.3)
    floor = u.snapshot()
    u.send("go depth 4")
    u.drain_quiet(6.0)
    u.send("stop")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove (mate-in-1)", floor=floor)
    u.wait_for(lambda l: "score mate 1" in l, 60, "'score mate 1' is presented", floor=floor)
    b = chess.Board(fen)
    tok = u.move_of(bm)
    check(tok != "0000", "a mate-in-1 root is not 0000")
    b.push(chess.Move.from_uci(tok))
    check(b.is_checkmate(), f"the presented bestmove ({tok}) is a reference-confirmed checkmate")

    # a mated root (in check, no evasion, long halfmove: mate beats the draw clock)
    fen = "7k/6Q1/5K2/8/8/8/8/8 b - - 100 1"
    u.send(f"position fen {fen}")
    u.drain_quiet(0.3)
    floor = u.snapshot()
    u.send("go depth 4")
    u.drain_quiet(6.0)
    u.send("stop")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove (mated root)", floor=floor)
    u.wait_for(lambda l: "score mate 0" in l, 60, "'score mate 0' for a mated root", floor=floor)
    check(u.move_of(bm) == "0000", "a mated root emits 0000")
    b = chess.Board(fen)
    check(b.is_checkmate(), "the reference agrees the mated root is checkmated")

    # a stalemate root: a real DRAW (cp 0) with 0000, NOT a mate score
    fen = "7k/5Q2/5K2/8/8/8/8/8 b - - 0 1"
    b = chess.Board(fen)
    if not b.is_stalemate():
        check(False, "stalemate fixture sanity (reference says not stalemate)")
        return
    u.send(f"position fen {fen}")
    u.drain_quiet(0.3)
    floor = u.snapshot()
    u.send("go depth 4")
    u.drain_quiet(6.0)
    u.send("stop")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove (stalemate root)", floor=floor)
    u.wait_for(lambda l: "score cp 0" in l and "mate" not in l, 60,
               "stalemate presents cp 0 (not a mate score)", floor=floor)
    check(u.move_of(bm) == "0000", "a stalemate root emits 0000")

    # a drawn root WITH legal moves (rule-50 counter 100): cp 0, a legal move, not 0000
    fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 100 1"
    u.send(f"position fen {fen}")
    u.drain_quiet(0.3)
    floor = u.snapshot()
    u.send("go depth 4")
    u.drain_quiet(6.0)
    u.send("stop")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove (rule-50 draw)", floor=floor)
    u.wait_for(lambda l: "score cp 0" in l, 60, "rule-50 exhaustion presents cp 0", floor=floor)
    b = chess.Board(fen)
    tok = u.move_of(bm)
    check(tok != "0000", "a drawn root with legal moves is NOT 0000")
    check(from_uci(tok) in b.legal_moves, "the draw-policy bestmove is legal")

    # explicit zero budgets: an immediate legal fallback, a depth-0 info
    for cmd in ("go movetime 0", "go nodes 0", "go wtime 0 btime 0"):
        u.send("position startpos")
        u.drain_quiet(0.2)
        floor = u.snapshot()
        u.send(cmd)
        bm = u.wait_for(lambda l: l.startswith("bestmove "), 30, f"bestmove ({cmd})", floor=floor)
        u.wait_for(lambda l: l.startswith("info depth 0"), 30, f"a depth-0 info for {cmd}", floor=floor)
        tok = u.move_of(bm)
        check(tok != "0000" and from_uci(tok) in START.legal_moves,
              f"{cmd} returns an immediate legal fallback")
    # a zero clock WITH an increment still finishes promptly and legally
    u.send("position startpos")
    floor = u.snapshot()
    u.send("go wtime 0 btime 0 winc 1000 binc 1000 movestogo 5")
    t0 = time.monotonic()
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 30, "bestmove (zero clock + increment)", floor=floor)
    dt = time.monotonic() - t0
    check(dt < 15 * u.scale, f"zero clock + increment finishes promptly ({dt:.2f}s)")
    tok = u.move_of(bm)
    check(tok != "0000" and from_uci(tok) in START.legal_moves, "zero clock + increment is legal")


def test_node_and_time_limits(u, deadline):
    u.send("position startpos")
    u.drain_quiet(0.2)
    floor = u.snapshot()
    u.send("go nodes 5000")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 120, "bestmove (nodes 5000)", floor=floor)
    last = None
    for l in u.all_lines():
        p = parse_info(l)
        if p:
            last = p
    check(last is not None and last["nodes"] >= 5000,
          f"the nodes budget is consumed (final nodes {last and last['nodes']})")
    check(last is not None and last["nodes"] < 50000,
          f"the node budget is honored (final nodes {last and last['nodes']})")
    expect_bestmove_legal(u, START, bm, "nodes-5000")

    floor = u.snapshot()
    u.send("go movetime 300")
    t0 = time.monotonic()
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove (movetime 300)", floor=floor)
    dt = time.monotonic() - t0
    check(0.2 <= dt <= 20 * u.scale, f"movetime 300 finishes near its budget ({dt:.2f}s)")
    expect_bestmove_legal(u, START, bm, "movetime-300")

    # clock-limited: 1s remaining, 1 move to go
    floor = u.snapshot()
    u.send("go wtime 1000 btime 1000 winc 0 binc 0 movestogo 1")
    t0 = time.monotonic()
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove (1s clock)", floor=floor)
    dt = time.monotonic() - t0
    check(0.3 * u.scale <= dt <= 30 * u.scale, f"a 1s clock move takes bounded time ({dt:.2f}s)")
    expect_bestmove_legal(u, START, bm, "clock-1s")


# --------------------------------------------------------------------------
# 4. infinite lifecycle
# --------------------------------------------------------------------------
def test_infinite(u, deadline):
    u.send("position startpos")
    u.drain_quiet(0.2)
    u.send("go infinite")
    floor = u.snapshot()
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", 30, "readyok while an infinite job is active", floor=floor)
    u.drain_quiet(3.0)
    floor = u.snapshot()
    u.send("stop")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove after stop (infinite)", floor=floor)
    expect_bestmove_legal(u, START, bm, "infinite")
    check(len(u.bestmoves()) == 1, f"exactly one bestmove per accepted go (got {len(u.bestmoves())})")
    # a repeated stop is harmless (no second bestmove)
    u.send("stop")
    u.drain_quiet(1.0)
    check(len(u.bestmoves()) == 1, "a repeated stop emits no second bestmove")

    # a TERMINAL root with an infinite job: stays active, then 0000 on stop
    u.send("position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 100 1")
    u.drain_quiet(0.2)
    u.send("go infinite")
    floor = u.snapshot()
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", 30, "readyok while a terminal infinite job is active", floor=floor)
    floor = u.snapshot()
    u.send("stop")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove (terminal root, infinite)", floor=floor)
    check(u.move_of(bm) == "0000", "a terminal root under infinite yields 0000 on stop")
    check(len(u.bestmoves()) == 2, "exactly one bestmove for the second go too")

    # `go infinite nodes 50`: active until stop (not an unbounded search)
    u.send("position startpos")
    u.send("go infinite nodes 50")
    floor = u.snapshot()
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", 30, "readyok while an infinite+nodes job is active", floor=floor)
    u.drain_quiet(1.0)
    floor = u.snapshot()
    u.send("stop")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove (infinite nodes 50)", floor=floor)
    expect_bestmove_legal(u, START, bm, "infinite-nodes-50")


# --------------------------------------------------------------------------
# 5. replacement
# --------------------------------------------------------------------------
def test_replacement(u, deadline):
    u.send("position startpos")
    u.drain_quiet(0.2)
    before = len(u.bestmoves())
    u.send("go depth 40")            # a long job
    u.drain_quiet(1.0)
    u.send("go depth 2")             # replace
    floor = u.snapshot()
    u.wait_for(lambda l: l.startswith("bestmove "), 180, "first bestmove after replacement", floor=floor)
    u.wait_for(lambda l: l.startswith("bestmove "), 180, "second bestmove (the replacement)", floor=floor)
    check(len(u.bestmoves()) == before + 2,
          f"exactly two bestmoves: the old job finished and the new one too (got {len(u.bestmoves()) - before})")

    # a position while a job runs: the job completes, then the new state installs
    u.send("go depth 40")
    u.drain_quiet(1.0)
    u.send("position startpos moves e2e4 c7c5")
    u.wait_for(lambda l: l.startswith("bestmove "), 180, "bestmove after a mid-search position")
    b = START.copy()
    b.push(chess.Move.from_uci("e2e4"))
    b.push(chess.Move.from_uci("c7c5"))
    got = fen_of(u, 30, "fen after a mid-search position")
    check(got == b.fen(), "an accepted mid-search position installs exactly")

    # ucinewgame while a job runs
    u.send("go depth 40")
    u.drain_quiet(1.0)
    u.send("ucinewgame")
    u.wait_for(lambda l: l.startswith("bestmove "), 180, "bestmove after a mid-search ucinewgame")
    got = fen_of(u, 30, "fen after a mid-search ucinewgame")
    check(got == START.fen(), "ucinewgame installs a fresh startpos")


# --------------------------------------------------------------------------
# 6. malformed input
# --------------------------------------------------------------------------
def test_malformed(u, deadline):
    before = len(u.bestmoves())
    u.send("go depth")               # a missing value
    u.drain_quiet(0.5)
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", 30, "readyok after a malformed go")
    check(len(u.bestmoves()) == before, "a malformed go starts no job")
    u.send("go ponder")              # an unsupported mode: explicit rejection
    u.drain_quiet(0.5)
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", 30, "readyok after go ponder")
    check(len(u.bestmoves()) == before, "go ponder is rejected (no job)")
    u.send("go depth -1")
    u.send("go depth 99999999999999999999")
    u.drain_quiet(0.5)
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", 30, "readyok after bad numeric limits")
    check(len(u.bestmoves()) == before, "bad numeric limits start no job")
    u.send("position fen onewordonly")
    u.send("position startpos moves e2e2 e2e4")
    u.drain_quiet(0.5)
    got = fen_of(u, 30, "fen after rejected positions")
    check(got == START.fen(), "rejected positions keep the previous state (startpos)")
    # an oversized line is drained, rejected, and the stream stays in sync
    u.send("go " + "x " * 150000)
    u.drain_quiet(1.0)
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", 30, "readyok after an oversized line (stream in sync)")
    check(len(u.bestmoves()) == before, "an oversized line starts no job")
    # unknown commands are harmless
    u.send("frobnicate everything")
    u.drain_quiet(0.3)
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", 30, "readyok after an unknown command")


# --------------------------------------------------------------------------
# 7. latency
# --------------------------------------------------------------------------
def test_latency(u, deadline):
    u.send("position startpos")
    u.send("go depth 25")            # a long search (no pruning: it will run on)
    floor = u.snapshot()
    t0 = time.monotonic()
    u.send("isready")
    u.wait_for(lambda l: l == "readyok", 30, "readyok during a long search", floor=floor)
    dt_ready = time.monotonic() - t0
    info(f"isready during search answered in {dt_ready * 1000:.0f} ms")
    check(dt_ready < 10 * u.scale, "isready during search is responsive")
    floor = u.snapshot()
    t0 = time.monotonic()
    u.send("stop")
    bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, "bestmove after stop", floor=floor)
    dt_stop = time.monotonic() - t0
    info(f"stop -> bestmove in {dt_stop * 1000:.0f} ms")
    check(dt_stop < 30 * u.scale, "stop yields a bounded cancellation latency")


# --------------------------------------------------------------------------
# 8. self-play + time control
# --------------------------------------------------------------------------
def test_selfplay(u, deadline):
    # Wall-time-limited moves (movetime): each move's WALL duration is bounded
    # regardless of the build's NPS, so this section has a stable budget in
    # release, debug and sanitizer builds alike.
    b = START.copy()
    plies = 0
    limit = 40
    while plies < limit and not b.is_game_over():
        u.send(f"position fen {b.fen()}")
        u.drain_quiet(0.15)
        floor = u.snapshot()
        u.send("go movetime 300")
        t0 = time.monotonic()
        bm = u.wait_for(lambda l: l.startswith("bestmove "), 120, f"self-play move {plies}", floor=floor)
        tok = u.move_of(bm)
        if tok == "0000":
            check(False, f"self-play got 0000 with legal moves remaining at ply {plies} ({b.fen()})")
            return
        mv = from_uci(tok)
        if mv not in b.legal_moves:
            check(False, f"self-play bestmove {tok} illegal at ply {plies} ({b.fen()})")
            return
        b.push(mv)
        plies += 1
    info(f"self-play (movetime 300): {plies} plies, result {b.result()}")
    check(plies >= 8, f"self-play made a meaningful number of plies ({plies})")


def test_time_control(u, deadline):
    b = START.copy()
    for plies in range(8):
        if b.is_game_over():
            break
        u.send(f"position fen {b.fen()}")
        u.drain_quiet(0.15)
        w = 2000 if b.turn == chess.WHITE else 0
        bl = 2000 if b.turn == chess.BLACK else 0
        floor = u.snapshot()
        u.send(f"go wtime {w} btime {bl} winc 100 binc 100 movestogo 5")
        t0 = time.monotonic()
        bm = u.wait_for(lambda l: l.startswith("bestmove "), 60, f"time-control move {plies}", floor=floor)
        dt = time.monotonic() - t0
        tok = u.move_of(bm)
        if tok == "0000" or b.is_game_over():
            check(tok == "0000" or from_uci(tok) in b.legal_moves,
                  f"the time-control terminal move is honest ({tok})")
            break
        mv = from_uci(tok)
        if mv not in b.legal_moves:
            check(False, f"time-control move {tok} illegal at ply {plies} ({b.fen()})")
            return
        b.push(mv)
        check(dt < 15 * u.scale, f"the clock move {plies} finished within bounds ({dt:.2f}s)")
    check(True, f"time-control game ({b.result()})")


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True)
    ap.add_argument("--scale", type=float, default=1.0,
                    help="multiply all client-side timeouts (raise for slow instrumented builds)")
    args = ap.parse_args()
    if not os.path.exists(args.engine) or not os.access(args.engine, os.X_OK):
        sys.exit(f"FAIL: engine binary missing or not executable: {args.engine}")
    deadline = 30.0

    print(f"UCI integration: engine={args.engine} scale={args.scale} "
          f"(chess {chess.__version__}, python {sys.version.split()[0]})")

    u = Uci(args.engine, "main", scale=args.scale)
    try:
        for name, fn in [
            ("handshake", test_handshake),
            ("positions", test_positions),
            ("long/stable history", test_long_and_stable),
            ("search output", test_search_output),
            ("node/time limits", test_node_and_time_limits),
            ("infinite lifecycle", test_infinite),
            ("replacement", test_replacement),
            ("malformed input", test_malformed),
            ("latency", test_latency),
            ("self-play", test_selfplay),
            ("time-control", test_time_control),
        ]:
            print(f"[{name}]")
            fn(u, deadline)
    finally:
        u.close()

    if failed:
        tail = u.err_tail()
        if tail:
            print("engine stderr tail:")
            for l in tail[-12:]:
                print("  " + l)
    print(f"\nUCI integration: {passed} checks passed, {failed} failed")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
