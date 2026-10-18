/* QwenChess: strict standard-FEN load/emit for the one Position.
 *
 * The loader is a pure reader: it parses a well-formed 6-field FEN into a scratch
 * Position, rebuilds its derived caches, runs pos_validate, and only on full
 * success commits it to *pos (the caller's Position is left untouched on failure,
 * with a static-backed reason supplied). Emission is the exact inverse at the level
 * of logical state. It preserves the recorded en-passant square and normalizes
 * placement runs and castling order. Canonical FEN text round-trips byte-for-byte.
 * No move generation, no Network, no heap.
 *
 * FEN grammar accepted (see docs): <placement> <side> <castling> <en-passant>
 * <halfmove> <fullmove>, single spaces, six non-empty fields.
 *
 * There is exactly one parser and one emitter (the bounded primary overloads). The
 * legacy pointer overloads validate their null/signed cases and delegate to the
 * same primary; the field helpers below are shared. The `why`/`error` diagnostics
 * are backed by string literals, so the returned std::string_view outlives the call.
 */
#include "position/fen.hpp"

#include "position/position.hpp"
#include "core/types.hpp"
#include "core/square.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

namespace qwc {

/* ---- character <-> Piece (cold path; ASCII is the FEN character set) ---- */

/* Map a FEN piece character to a Piece value, or -1 if it is not a piece letter.
 * Uppercase letters are white, lowercase are black. */
static int piece_from_char(char c) {
  Color color;
  char uc;
  if (c >= 'a' && c <= 'z') { color = Color::BLACK; uc = (char)(c - 'a' + 'A'); }
  else if (c >= 'A' && c <= 'Z') { color = Color::WHITE; uc = c; }
  else return -1;
  PieceType t;
  switch (uc) {
    case 'P': t = PieceType::PAWN;   break;
    case 'N': t = PieceType::KNIGHT; break;
    case 'B': t = PieceType::BISHOP; break;
    case 'R': t = PieceType::ROOK;   break;
    case 'Q': t = PieceType::QUEEN;  break;
    case 'K': t = PieceType::KING;   break;
    default:  return -1;
  }
  return (int)(to_under(t) | (to_under(color) << 3));   /* type (1..6) | color (0/8) */
}

/* Map a Piece (1..15) to its FEN letter. */
static char char_from_piece(Piece p) {
  static constexpr std::array<char, PIECE_TYPE_NB> upper{ ' ', 'P', 'N', 'B', 'R', 'Q', 'K' };
  char c = upper[to_under(p) & 0x7u];
  return (p >= Piece::B_PAWN) ? (char)(c | 0x20) : c;   /* lowercase if black */
}

/* ---- 6-field split (single spaces, six non-empty fields) ---- */

/* Split a FEN into six bounded fields. The split is driven by the view's size, not
 * by an assumed NUL terminator, so a valid FEN that is a non-terminated buffer (or
 * a slice of a larger one) is handled. Requires exactly five single spaces and six
 * non-empty fields (so no leading/trailing/double spaces). Each field[i] borrows the
 * view's storage. Sets *why on failure. Never mutates the input. */
static int split_fields(std::string_view text, std::array<std::string_view, 6> *fields,
                        std::string_view *why) {
  int nsp = 0;
  for (char c : text)
    if (c == ' ')
      nsp++;
  if (nsp != 5) { if (why) *why = "FEN must have exactly six fields"; return 0; }
  std::size_t start = 0;
  for (int i = 0; i < 6; i++) {
    std::size_t sp = start;
    while (sp < text.size() && text[sp] != ' ') sp++;
    std::size_t len = sp - start;
    if (len == 0) { if (why) *why = "empty FEN field (edge or double space)"; return 0; }
    (*fields)[i] = text.substr(start, len);
    start = (sp < text.size()) ? sp + 1 : sp;   /* advance past the space, or stop at the end */
  }
  return 1;
}

/* ---- field: placement (8 ranks, rank 8 first) ---- */

/* Parse the placement field into *mailbox. Eight ranks separated by '/', rank 8
 * first; each rank spans exactly 8 squares (digits 1-8 = empty squares, letters
 * = pieces). The field is the bounded (s, len) range; only s[0..len) is read.
 * Returns 1 on success, 0 (and *why) on failure. */
static int parse_placement(const char *s, std::size_t len, Piece *mailbox, std::string_view *why) {
  std::fill(mailbox, mailbox + SQ_NB, Piece::NO_PIECE);
  int rank = RANK_8;   /* FEN lists the 8th rank first, down to the 1st */
  int col  = 0;        /* squares placed on the current rank (0..8) */
  for (std::size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c == '/') {
      if (col != 8) { if (why) *why = "a placement rank does not span 8 squares"; return 0; }
      if (rank == RANK_1) { if (why) *why = "placement has more than 8 ranks"; return 0; }
      rank--; col = 0;
      continue;
    }
    if (c >= '1' && c <= '8') {
      int k = c - '0';               /* 1..8 empty squares */
      if (col + k > 8) { if (why) *why = "a placement rank exceeds 8 squares"; return 0; }
      col += k;
    } else {
      int pc = piece_from_char(c);
      if (pc < 0) { if (why) *why = "invalid placement character"; return 0; }
      if (col > 7) { if (why) *why = "a placement rank exceeds 8 squares"; return 0; }
      mailbox[square_of((Square)col, (Square)rank)] = (Piece)pc;
      col++;
    }
  }
  if (rank != RANK_1 || col != 8) { if (why) *why = "placement does not fill 8 ranks of 8 squares"; return 0; }
  return 1;
}

/* ---- field: side ---- */
static int parse_side(const char *s, std::size_t len, Color *side, std::string_view *why) {
  if (len == 1 && s[0] == 'w') { *side = Color::WHITE; return 1; }
  if (len == 1 && s[0] == 'b') { *side = Color::BLACK; return 1; }
  if (why) *why = "side must be 'w' or 'b'";
  return 0;
}

/* ---- field: castling ("-" or any order of KQkq, no repeats) ---- */
static int parse_castling(const char *s, std::size_t len, u8 *cr, std::string_view *why) {
  *cr = 0;
  if (len == 1 && s[0] == '-')
    return 1;
  for (std::size_t i = 0; i < len; i++) {
    u8 bit;
    switch (s[i]) {
      case 'K': bit = CR_WK; break;
      case 'Q': bit = CR_WQ; break;
      case 'k': bit = CR_BK; break;
      case 'q': bit = CR_BQ; break;
      default: if (why) *why = "invalid castling character"; return 0;
    }
    if (*cr & bit) { if (why) *why = "duplicate castling right"; return 0; }
    *cr |= bit;
  }
  return 1;
}

/* ---- field: en-passant ("-" or a square) ----
 *
 * Sets *ep to the RECORDED target square (or NO_SQUARE for "-"). The square is
 * kept even if no capture is actually legal; structural validity (correct rank,
 * empty destination, the enemy pawn, a vacant double-push origin, zero halfmove
 * clock) is enforced downstream by pos_validate. Only the *spelling* is checked
 * here. */
static int parse_ep(const char *s, std::size_t len, Square *ep, std::string_view *why) {
  if (len == 1 && s[0] == '-') { *ep = NO_SQUARE; return 1; }
  if (len != 2) { if (why) *why = "en-passant must be '-' or a two-letter square"; return 0; }
  if (s[0] < 'a' || s[0] > 'h') { if (why) *why = "en-passant file is out of range"; return 0; }
  if (s[1] < '1' || s[1] > '8') { if (why) *why = "en-passant rank is out of range"; return 0; }
  *ep = square_of((Square)(s[0] - 'a'), (Square)(s[1] - '1'));
  return 1;
}

/* ---- fields: halfmove / fullmove (canonical non-negative base-10) ---- */

/* Parse a canonical, non-negative base-10 integer into *out, bounded by [0,max]:
 * at most `maxdigits` digits, all digits, no sign, and no leading zero (except
 * the single digit "0"). The field is the bounded (s, len) range; only s[0..len)
 * is read. Returns 1 on success, 0 (and *why) otherwise. */
static int parse_uint(const char *s, std::size_t len, int maxdigits, int max, int *out,
                      std::string_view *why, const char *field) {
  if (len == 0 || len > (std::size_t)maxdigits) { if (why) *why = field; return 0; }
  if (s[0] == '0' && len > 1) { if (why) *why = "leading zero in counter"; return 0; }
  int v = 0;
  for (std::size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') { if (why) *why = "counter is not a plain number"; return 0; }
    v = v * 10 + (s[i] - '0');
    if (v > max) { if (why) *why = "counter out of range"; return 0; }
  }
  *out = v;
  return 1;
}

/* ---- primary: load (bounded) ---- */

bool fen_load(Position& pos, std::string_view text, std::string_view* error) {
  if (error) *error = std::string_view{};   /* cleared on entry */
  if (text.empty()) { if (error) *error = "empty FEN"; return false; }
  /* A NUL inside the explicitly supplied range is input data (a terminator that
   * was accidentally included) and is rejected; the parser is sized by the view. */
  for (char c : text)
    if (c == '\0') { if (error) *error = "NUL byte in FEN input"; return false; }

  std::array<std::string_view, 6> fields;
  Position tmp;
  int v;
  /* `tmp` is zero-initialized, so tmp.history is nullptr -- a successful load
   * therefore starts a FRESH history (the prior chain is abandoned but its
   * caller-owned records are left in place); a failed load leaves `pos` -- and its
   * existing history pointer -- untouched. */
  tmp = Position{};

  if (!split_fields(text, &fields, error)) return false;
  if (!parse_placement(fields[0].data(), fields[0].size(), tmp.mailbox.data(), error)) return false;
  if (!parse_side(fields[1].data(), fields[1].size(), &tmp.side, error)) return false;
  if (!parse_castling(fields[2].data(), fields[2].size(), &tmp.cr, error)) return false;
  if (!parse_ep(fields[3].data(), fields[3].size(), &tmp.ep_sq, error)) return false;
  if (!parse_uint(fields[4].data(), fields[4].size(), 5, POS_HM_MAX, &v, error, "halfmove field")) return false;
  tmp.halfmove = (HalfMoveClock)v;   /* checked before narrowing to u16 */
  if (!parse_uint(fields[5].data(), fields[5].size(), 5, 0xFFFF, &v, error, "fullmove field")) return false;
  if (v < 1) { if (error) *error = "fullmove must be at least 1"; return false; }
  tmp.fullmove = (FullMoveNumber)v;  /* v <= 0xFFFF: fits a u16 */

  pos_rebuild(&tmp);
  {
    const char *vwhy = nullptr;
    if (!pos_validate(&tmp, &vwhy)) {
      if (error) *error = std::string_view(vwhy ? vwhy : "position failed validation");
      return false;
    }
  }
  pos = tmp;   /* commit only on complete success */
  if (error) *error = std::string_view{};   /* success leaves the error empty */
  return true;
}

/* ---- adapter: load (C-string) ----
 *
 * The legacy entry point. It validates the null cases BEFORE constructing the view,
 * builds a NUL-terminator-excluding view via strlen (a const char* input requires a
 * valid terminator), and delegates to the primary. `why` receives the static-backed
 * diagnostic from the primary on failure, or nullptr on success. */
int fen_load(Position *pos, const char *fen, const char **why) {
  if (why) *why = nullptr;
  if (pos == nullptr)
    return 0;
  if (fen == nullptr) { if (why) *why = "nullptr FEN"; return 0; }
  std::string_view err;
  bool ok = fen_load(*pos, std::string_view(fen, std::strlen(fen)), &err);
  if (why) *why = ok ? nullptr : err.data();   /* err is static-backed when nonempty */
  return ok ? 1 : 0;
}

/* ---- emission helpers (shared by the primary) ---- */

/* Number of decimal digits of a non-negative value (0 -> 1). */
static int num_digits(int v) {
  int d = 1;
  while (v >= 10) { v /= 10; d++; }
  return d;
}

/* Total rendered FEN length in characters (excluding the NUL). */
static int fen_len(const Position *pos) {
  int len = 0;
  for (int r = RANK_8; r >= RANK_1; r--) {
    int run = 0, ranklen = 0;
    for (int f = FILE_A; f < FILE_NB; f++) {
      if (pos->mailbox[square_of((Square)f, (Square)r)] == Piece::NO_PIECE) { run++; continue; }
      if (run) { ranklen += 1; run = 0; }   /* a run of 1..8 empty squares = 1 digit */
      ranklen++;
    }
    if (run) ranklen += 1;
    len += ranklen;
    if (r != RANK_1) len += 1;              /* the '/' between ranks */
  }
  len += 5;                                 /* the five single spaces */
  len += 1;                                 /* side */
  len += (pos->cr ? popcount((u64)pos->cr) : 1);   /* KQkq (1..4) or '-' */
  len += (pos->ep_sq < SQ_NB ? 2 : 1);     /* square or '-' */
  len += num_digits((int)pos->halfmove);
  len += num_digits((int)pos->fullmove);
  return len;
}

/* Write the decimal digits of a non-negative value into out at *i (advancing it). */
static void write_num(char *out, int *i, int v) {
  std::array<char, 8> buf{};
  int n = 0;
  do { buf[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
  for (int k = n - 1; k >= 0; k--)
    out[(*i)++] = buf[k];
}

/* ---- primary: emit (bounded) ---- */

bool fen_emit(const Position& pos, std::span<char> output) {
  if (output.empty())
    return false;   /* no room for any byte: fail, leave the buffer untouched */
  if (!pos_validate(&pos, nullptr))
    return false;   /* invalid input: do not write a partial or malformed FEN */
  int total = fen_len(&pos);
  if ((std::size_t)(total + 1) > output.size())
    return false;   /* buffer too small to hold the FEN plus its terminator */
  char *out = output.data();
  int i = 0;

  for (int r = RANK_8; r >= RANK_1; r--) {
    int run = 0;
    for (int f = FILE_A; f < FILE_NB; f++) {
      Piece p = pos.mailbox[square_of((Square)f, (Square)r)];
      if (p == Piece::NO_PIECE) { run++; continue; }
      if (run) { out[i++] = (char)('0' + run); run = 0; }
      out[i++] = char_from_piece(p);
    }
    if (run) out[i++] = (char)('0' + run);
    if (r != RANK_1) out[i++] = '/';
  }

  out[i++] = ' ';
  out[i++] = (pos.side == Color::WHITE) ? 'w' : 'b';

  out[i++] = ' ';
  if (pos.cr == 0) out[i++] = '-';
  else {
    if (pos.cr & CR_WK) out[i++] = 'K';
    if (pos.cr & CR_WQ) out[i++] = 'Q';
    if (pos.cr & CR_BK) out[i++] = 'k';
    if (pos.cr & CR_BQ) out[i++] = 'q';
  }

  out[i++] = ' ';
  if (pos.ep_sq >= SQ_NB) out[i++] = '-';
  else { out[i++] = (char)('a' + (pos.ep_sq & 7)); out[i++] = (char)('1' + (pos.ep_sq >> 3)); }

  out[i++] = ' ';
  write_num(out, &i, (int)pos.halfmove);
  out[i++] = ' ';
  write_num(out, &i, (int)pos.fullmove);

  out[i] = '\0';   /* total+1 <= output.size() was checked, so i+1 is in bounds */
  return true;
}

/* ---- adapter: emit (buffer) ----
 *
 * The legacy entry point. It rejects a null `out`/`pos` and a nonpositive (signed)
 * `outsz` BEFORE converting to a size_t, then delegates to the primary. */
int fen_emit(const Position *pos, char *out, int outsz) {
  if (out == nullptr || pos == nullptr || outsz <= 0)
    return 0;
  return fen_emit(*pos, std::span<char>(out, (std::size_t)outsz)) ? 1 : 0;
}

}  // namespace qwc
