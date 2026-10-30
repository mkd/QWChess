#!/usr/bin/env python3
"""QwenChess independent move-generation differential (drives tools/position_probe).

Uses the pinned python `chess` 1.11.2 to compute the expected moves, legal walks and
FENs INDEPENDENTLY, encodes them into the QWChess move layout, drives the
QWChess-linked oracle probe, and compares the complete results. It fails at the first
mismatch with the root FEN, seed, ply, replayable move prefix and the expected/actual
difference -- it never drops a failing position or adjusts the expected value to match
the engine.

The encoded QWChess move layout is matched exactly: from bits 0-5, to bits 6-11,
flags bits 12-14 (EP=1, promo=2, castle=4), promo bits 16-18 (N/B/R/Q = 2/3/4/5,
else 0). For orthodox castles the king's g/c destination is mapped to the home-rook
h/a origin. EP and promo payloads are identified from the reference board/move; the
engine's output is never used to manufacture an expected value.

FENs are compared on all six fields using `board.fen(en_passant="fen")`, which emits
the recorded en-passant square (matching QWChess's recorded-EP contract; the default
FEN omits uncapturable EP records). Ordinary counters are kept intact during replay.
"""
import argparse
import random
import subprocess
import sys

try:
    import chess
except ImportError as e:
    sys.exit(f"the `chess` package is required (pinned: chess==1.11.2): {e}")

SEED = 0xC0FFEE                     # fixed, recorded seed for the seeded walks
# QWChess promo payload per reference PieceType. QWChess is 1-based Stockfish-mirroring
# (KNIGHT=2, BISHOP=3, ROOK=4, QUEEN=5); chess is 0-based, so key by the PieceType
# objects rather than assuming an offset.
PROMO = {chess.KNIGHT: 2, chess.BISHOP: 3, chess.ROOK: 4, chess.QUEEN: 5}
CASTLE = 4
EP = 1
PROMO_FLAG = 2
# Reverse mapping (QWChess promo payload -> reference PieceType) for replaying moves.
PROMO_REV = {2: chess.KNIGHT, 3: chess.BISHOP, 4: chess.ROOK, 5: chess.QUEEN}

# A deterministic total order over reference moves (chess 1.11.2 Move is not orderable).
def mkey(m):
    return (m.from_square, m.to_square, m.promotion or "")

# The standard perft appendix roots (also used as short-walk roots).
STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
SPECIAL_ROOTS = [
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
]

# Deliberate targeted lines: (name, fen, [SAN, ...]). An empty SAN list is a root-only
# line (used for mate/stalemate fixtures, which have no legal moves).
TARGETED = [
    ("W_ks_castle",   "4k3/8/8/8/8/8/8/4K2R w K - 0 1",  ["O-O"]),
    ("W_qs_castle",   "4k3/8/8/8/8/8/8/R3K2R w Q - 0 1",     ["O-O-O"]),
    ("B_ks_castle",   "4k2r/8/8/8/8/8/8/4K3 b k - 0 1",      ["O-O"]),
    ("B_qs_castle",   "r3k2r/8/8/8/8/8/8/4K3 b q - 0 1",    ["O-O-O"]),
    ("EP_capture",    "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", ["exd6"]),
    ("W_promo_Q",     "8/P7/8/8/8/8/8/4K2k w - - 0 1",  ["a8=Q"]),
    ("W_promo_N",     "8/P7/8/8/8/8/8/4K2k w - - 0 1",    ["a8=N"]),
    ("W_promo_B",     "8/P7/8/8/8/8/8/4K2k w - - 0 1",     ["a8=B"]),
    ("W_promo_R",     "8/P7/8/8/8/8/8/4K2k w - - 0 1",      ["a8=R"]),
    ("B_underpromo",  "4K2k/8/8/8/8/8/p7/8 b - - 0 1",   ["a1=n"]),
    ("W_cap_promo",   "n6k/1P6/8/8/8/8/8/4K3 w - - 0 1", ["bxa8=Q"]),
    ("check_evasion", "k7/8/8/8/8/8/8/R6K b - - 0 1",    ["Kb8"]),
    ("mate",          "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1",  []),
    ("stalemate",     "k7/8/1Q6/5K2/8/8/8/8 b - - 0 1", []),
    ("pinned_ep",     "4r2k/8/8/3pP3/8/8/8/4K3 w - d6 0 1", ["Ke2"]),
    # A genuine double-push record whose captor is absent: the double-pushed enemy pawn is
    # present (so pos_validate accepts it) but no friendly pawn can take, so the ep is
    # recorded yet uncapturable (canonical file -1). The ep rank (6) means black just
    # double-pushed, so it is white to move; a subsequent non-pawn move clears it.
    ("no_captor_ep",  "4k3/8/8/3p4/8/8/8/4K3 w - d6 0 1", ["Ke2"]),
    ("doublepush_ep", "K6k/8/8/8/4p3/8/3P4/8 w - - 0 1",   ["d4", "exd3"]),
    ("rights_king",   "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", ["Ke2"]),
    ("rights_rook",   "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", ["Ra2"]),
]


def encode(board, m):
    """Independently encode a reference move into the QWChess u32 layout.

    from bits 0-5, to bits 6-11, flags bits 12-14 (EP=1, promo=2, castle=4), promo
    bits 16-18 (N/B/R/Q = 2/3/4/5, else 0). For orthodox castles the king's g/c
    destination (as reported by the reference move) is mapped to the home-rook h/a
    origin. EP and promo are identified from the reference board/move; the engine
    output is never used to manufacture an expected value.
    """
    frm = m.from_square          # python-chess Square value == QWChess square (a1=0, rank*8+file)
    to = m.to_square
    flags = 0
    promo = 0
    if board.is_castling(m):
        to = (frm & 0x38) | (7 if (to & 7) == 6 else 0)   # king rank -> home-rook h/a file
        flags |= CASTLE
    elif board.is_en_passant(m):
        flags |= EP
    elif m.promotion is not None:
        flags |= PROMO_FLAG
        promo = PROMO[m.promotion]
    return frm | (to << 6) | (flags << 12) | (promo << 16)


def legal_set(board):
    """The complete, deduplicated, sorted encoded legal set (the reference)."""
    seen = set()
    out = []
    for m in board.legal_moves:
        e = encode(board, m)
        if e in seen:
            sys.exit(f"reference duplicate legal move {e:#x} in {board.fen()}")
        seen.add(e)
        out.append(e)
    out.sort()
    return out


def fen6(board):
    """All six FEN fields, using the recorded-EP convention (en_passant='fen')."""
    return board.fen(en_passant="fen").split()


def walk(board, rng, max_plies):
    """A bounded seeded legal walk (returns ENCODED moves). Moves are sorted before
    selection; it stops only when there are no legal moves (a real absence, not a
    draw claim). Each move is encoded against the board state at that ply."""
    b = board.copy()
    line = []
    for _ in range(max_plies):
        moves = sorted(b.legal_moves, key=mkey)
        if not moves:
            break
        m = rng.choice(moves)
        line.append(encode(b, m))
        b.push(m)
    return line


def validate_root(fen):
    """A root FEN must be a well-formed standard position: 8 ranks, 8 columns each,
    exactly one white and one black king, and loadable. Fails fast naming the bad row.
    (The QWChess probe loads the same FEN; keeping both sides on a legal position
    avoids a spurious load mismatch.)"""
    b = chess.Board(fen)   # raises on gross structural issues
    for i, row in enumerate(fen.split()[0].split("/")):
        n = sum(int(c) if c.isdigit() else 1 for c in row)
        if n != 8:
            raise ValueError(f"rank {i} has {n} columns (need 8): {row!r} in {fen!r}")
    wk = len(b.pieces(chess.KING, chess.WHITE))
    bk = len(b.pieces(chess.KING, chess.BLACK))
    if wk != 1 or bk != 1:
        raise ValueError(f"root FEN must contain exactly one king each: {fen!r}")
    # Pawns never sit on the back ranks (QWChess pos_validate rejects this).
    for sq in b.pieces(chess.PAWN, chess.WHITE):
        if sq < 8:
            raise ValueError(f"root FEN has a white pawn on the 1st rank: {fen!r}")
    for sq in b.pieces(chess.PAWN, chess.BLACK):
        if sq >= 56:
            raise ValueError(f"root FEN has a black pawn on the 8th rank: {fen!r}")
    # A legal position never leaves the side NOT to move in check (QWChess pos_validate
    # enforces this; python-chess tolerates it, so check it here to match).
    flipped = b.copy()
    flipped.turn = not flipped.turn
    if flipped.is_check():
        raise ValueError(f"root FEN leaves the side not to move in check: {fen!r}")


def build_lines():
    """Generate all (root_fen, [encoded moves]) lines."""
    lines = []
    rng = random.Random(SEED)

    # Validate every distinct root FEN up front (a bad FEN fails fast, clearly).
    for fen in [STARTPOS, *SPECIAL_ROOTS, *(t[1] for t in TARGETED)]:
        validate_root(fen)

    # 32 seeded random walks from the startpos (up to 24 plies; stops at no-legal-moves).
    for _ in range(32):
        lines.append((STARTPOS, walk(chess.Board(STARTPOS), rng, 24)))
    # Short seeded walks from the special roots (up to 12 plies).
    for root in SPECIAL_ROOTS:
        for _ in range(8):
            lines.append((root, walk(chess.Board(root), rng, 12)))
    # Deliberate targeted lines (the SAN moves, encoded against the current board).
    for name, fen, sans in TARGETED:
        b = chess.Board(fen)
        moves = []
        for san in sans:
            m = b.parse_san(san)
            moves.append(encode(b, m))
            b.push_san(san)
        lines.append((fen, moves))
        # Apply every legal root move as a one-ply line (where useful: non-empty sets).
        rb = chess.Board(fen)
        for m in sorted(rb.legal_moves, key=mkey):
            lines.append((fen, [encode(rb, m)]))
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True, help="path to the position_probe binary")
    args = ap.parse_args()

    print(f"chess {chess.__version__} | seed {SEED:#x} | python {sys.version.split()[0]}")
    if chess.__version__ != "1.11.2":
        print(f"WARNING: expected chess 1.11.2, found {chess.__version__}")

    lines = build_lines()

    # Build the batch input: N, then per line the root FEN + the encoded moves.
    inp = [str(len(lines))]
    for fen, moves in lines:
        inp.append(fen)
        inp.append(" ".join(str(m) for m in moves))
    proc = subprocess.run([args.probe], input="\n".join(inp) + "\n",
                          capture_output=True, text=True)

    first_bad = None
    # Parse the probe's reported positions. The probe emits: P <line> <ply> then the
    # six FEN fields as separate tokens (placement turn castling ep halfmove fullmove),
    # then the complete legal set. The six fields are re-joined into the six-field FEN
    # (matching python-chess fen(en_passant="fen")), and the remainder are the moves.
    reported = {}
    for ln in proc.stdout.splitlines():
        parts = ln.split()
        if not parts or parts[0] != "P":
            continue
        line, ply = int(parts[1]), int(parts[2])
        if len(parts) < 3 + 6:
            first_bad = (first_bad or f"line {line} ply {ply}: short probe report "
                          f"(need 6 FEN fields, got {max(len(parts)-3, 0)})")
            continue
        reported[(line, ply)] = (parts[3:9], [int(x) for x in parts[9:]])

    if proc.returncode != 0:
        print(f"PROBE FAILED (exit {proc.returncode}):")
        print(proc.stderr)
        return 1

    total_positions = 0
    total_transitions = 0
    for idx, (fen, moves) in enumerate(lines):
        b = chess.Board(fen)
        for ply in range(len(moves) + 1):
            total_positions += 1
            if ply > 0:
                total_transitions += 1
            exp_fen = fen6(b)
            exp_legal = legal_set(b)
            key = (idx, ply)
            if key not in reported:
                first_bad = first_bad or f"line {idx} ply {ply}: probe did not report this position (root {fen})"
                break
            rep_fen, rep_legal = reported[key]
            # FEN: all six fields.
            if len(rep_fen) != 6:
                first_bad = first_bad or f"line {idx} ply {ply}: bad FEN arity ({rep_fen!r})"
                break
            for i, (e, r) in enumerate(zip(exp_fen, rep_fen)):
                if e != r:
                    first_bad = first_bad or (f"line {idx} ply {ply}: FEN field {i} differs: "
                                              f"expected {e!r} got {r!r} (root {fen})")
                    break
            else:
                # Legal set: complete, deduplicated, in any order (compared as sets).
                if len(set(rep_legal)) != len(rep_legal):
                    first_bad = first_bad or f"line {idx} ply {ply}: engine reported duplicate moves"
                    break
                if sorted(rep_legal) != exp_legal:
                    missing = sorted(set(exp_legal) - set(rep_legal))
                    extra = sorted(set(rep_legal) - set(exp_legal))
                    first_bad = first_bad or (f"line {idx} ply {ply}: legal-set mismatch "
                                              f"(missing {missing[:4]}, extra {extra[:4]}; "
                                              f"expected {len(exp_legal)} got {len(rep_legal)}; root {fen})")
                    break
            b = b.copy()
            if ply < len(moves):
                # Advance the reference board by the (already-verified) move: decode
                # the QWChess encoding back into a reference move and push it.
                frm = moves[ply] & 0x3F
                to = (moves[ply] >> 6) & 0x3F
                flags = (moves[ply] >> 12) & 0x7
                promo = (moves[ply] >> 16) & 0x7
                if flags & CASTLE:
                    # The encoded 'to' is the home-rook origin (h/a file); the king's
                    # actual destination is the g/c file on the same rank.
                    b.push(chess.Move(frm, (frm & 0x38) | (6 if (to & 7) == 7 else 2)))
                else:
                    m = chess.Move(frm, to)
                    if flags & PROMO_FLAG:
                        m.promotion = PROMO_REV[promo]
                    b.push(m)
    if first_bad:
        print(f"FAIL: {first_bad}")
        print(f"  (positions checked before failure: {total_positions})")
        return 1
    ok = total_positions >= 1000
    print(f"positions={total_positions} transitions={total_transitions} "
          f"lines={len(lines)} -> {'PASS' if ok else 'SHORTFALL (<1000 positions)'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
