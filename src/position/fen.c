/* QwenChess: strict standard-FEN load/emit for the one Position.
 *
 * The loader is a pure reader: it parses a well-formed 6-field FEN into a
 * scratch Position, rebuilds its derived caches, runs pos_validate, and only on
 * full success commits it to *pos (the caller's Position is left untouched on
 * failure, with *why set to a short, fielded reason). Emission is the exact
 * inverse at the level of logical state. It preserves the recorded en-passant
 * square and normalizes placement runs and castling order. Canonical FEN text
 * round-trips byte-for-byte. No move generation, no Network, no heap.
 *
 * FEN grammar accepted (see docs): <placement> <side> <castling> <en-passant>
 * <halfmove> <fullmove>, single spaces, six non-empty fields. Placement is eight
 * ranks (rank 8 first) separated by '/'; castling is "-" or any order of KQkq.
 */
#include "position/fen.h"
#include "position/position.h"
#include "core/types.h"
#include "core/square.h"
#include <string.h>

/* ---- character <-> Piece (cold path; ASCII is the FEN character set) ---- */

/* Map a FEN piece character to a Piece value, or -1 if it is not a piece letter.
 * Uppercase letters are white, lowercase are black. */
static int piece_from_char(char c) {
  Color color;
  char uc;
  if (c >= 'a' && c <= 'z') { color = BLACK; uc = (char)(c - 'a' + 'A'); }
  else if (c >= 'A' && c <= 'Z') { color = WHITE; uc = c; }
  else return -1;
  PieceType t;
  switch (uc) {
    case 'P': t = PAWN;   break;
    case 'N': t = KNIGHT; break;
    case 'B': t = BISHOP; break;
    case 'R': t = ROOK;   break;
    case 'Q': t = QUEEN;  break;
    case 'K': t = KING;   break;
    default:  return -1;
  }
  return (int)((u8)t | (u8)(color << 3));   /* type (1..6) | color (0/8) */
}

/* Map a Piece (1..15) to its FEN letter. */
static char char_from_piece(Piece p) {
  static const char upper[PIECE_TYPE_NB] = { ' ', 'P', 'N', 'B', 'R', 'Q', 'K' };
  char c = upper[p & 7];
  return (p >= B_PAWN) ? (char)(c | 0x20) : c;   /* lowercase if black */
}

/* ---- 6-field split (single spaces, six non-empty fields) ---- */

/* Split a FEN into six (pointer, length) fields. Requires exactly five single
 * spaces and six non-empty fields (so no leading/trailing/double spaces). Sets
 * *why on failure. Never mutates the input. */
static int split_fields(const char *fen, const char **f, size_t *n, const char **why) {
  int nsp = 0;
  for (const char *p = fen; *p; p++)
    if (*p == ' ')
      nsp++;
  if (nsp != 5) { if (why) *why = "FEN must have exactly six fields"; return 0; }
  const char *p = fen;
  for (int i = 0; i < 6; i++) {
    const char *sp;
    size_t len;
    f[i] = p;
    sp = p;
    while (*sp && *sp != ' ') sp++;
    len = (size_t)(sp - p);
    if (len == 0) { if (why) *why = "empty FEN field (edge or double space)"; return 0; }
    n[i] = len;
    p = (*sp) ? sp + 1 : sp;   /* advance past the space, or stop at the NUL */
  }
  return 1;
}

/* ---- field: placement (8 ranks, rank 8 first) ---- */

/* Parse the placement field into *mailbox. Eight ranks separated by '/', rank 8
 * first; each rank spans exactly 8 squares (digits 1-8 = empty squares, letters
 * = pieces). Returns 1 on success, 0 (and *why) on failure. */
static int parse_placement(const char *s, size_t len, u8 *mailbox, const char **why) {
  memset(mailbox, 0, SQ_NB);
  int rank = RANK_8;   /* FEN lists the 8th rank first, down to the 1st */
  int col  = 0;        /* squares placed on the current rank (0..8) */
  for (size_t i = 0; i < len; i++) {
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
      mailbox[square_of((Square)col, (Square)rank)] = (u8)pc;
      col++;
    }
  }
  if (rank != RANK_1 || col != 8) { if (why) *why = "placement does not fill 8 ranks of 8 squares"; return 0; }
  return 1;
}

/* ---- field: side ---- */
static int parse_side(const char *s, size_t len, Color *side, const char **why) {
  if (len == 1 && s[0] == 'w') { *side = WHITE; return 1; }
  if (len == 1 && s[0] == 'b') { *side = BLACK; return 1; }
  if (why) *why = "side must be 'w' or 'b'";
  return 0;
}

/* ---- field: castling ("-" or any order of KQkq, no repeats) ---- */
static int parse_castling(const char *s, size_t len, u8 *cr, const char **why) {
  *cr = 0;
  if (len == 1 && s[0] == '-')
    return 1;
  for (size_t i = 0; i < len; i++) {
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
static int parse_ep(const char *s, size_t len, Square *ep, const char **why) {
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
 * the single digit "0"). Returns 1 on success, 0 (and *why) otherwise. */
static int parse_uint(const char *s, size_t len, int maxdigits, int max, int *out,
                      const char **why, const char *field) {
  if (len == 0 || len > (size_t)maxdigits) { if (why) *why = field; return 0; }
  if (s[0] == '0' && len > 1) { if (why) *why = "leading zero in counter"; return 0; }
  int v = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') { if (why) *why = "counter is not a plain number"; return 0; }
    v = v * 10 + (s[i] - '0');
    if (v > max) { if (why) *why = "counter out of range"; return 0; }
  }
  *out = v;
  return 1;
}

/* ---- public: load ---- */

int fen_load(Position *pos, const char *fen, const char **why) {
  if (why) *why = NULL;
  if (pos == NULL)
    return 0;
  if (fen == NULL) { if (why) *why = "NULL FEN"; return 0; }

  const char *f[6];
  size_t n[6];
  Position tmp;
  int v;

  /* Parse every field into a scratch Position; on any failure *pos is left
   * untouched and *why names the offending field. */
  memset(&tmp, 0, sizeof tmp);
  if (!split_fields(fen, f, n, why)) return 0;
  if (!parse_placement(f[0], n[0], tmp.mailbox, why)) return 0;
  if (!parse_side(f[1], n[1], &tmp.side, why)) return 0;
  if (!parse_castling(f[2], n[2], &tmp.cr, why)) return 0;
  if (!parse_ep(f[3], n[3], &tmp.ep_sq, why)) return 0;
  if (!parse_uint(f[4], n[4], 5, POS_HM_MAX, &v, why, "halfmove field")) return 0;
  tmp.halfmove = (HalfMoveClock)v;   /* checked before narrowing to u16 */
  if (!parse_uint(f[5], n[5], 5, 0xFFFF, &v, why, "fullmove field")) return 0;
  if (v < 1) { if (why) *why = "fullmove must be at least 1"; return 0; }
  tmp.fullmove = (FullMoveNumber)v;  /* v <= 0xFFFF: fits a u16 */

  /* Rebuild the derived caches, then require the full local-admissibility
   * contract (kings, pawns, counts, check, castling backing, EP metadata, key).
   * Only a position that passes is committed. */
  pos_rebuild(&tmp);
  if (!pos_validate(&tmp, why)) return 0;
  *pos = tmp;
  return 1;
}

/* ---- public: emit ---- */

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
      if (pos->mailbox[square_of((Square)f, (Square)r)] == NO_PIECE) { run++; continue; }
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
  char buf[8];
  int n = 0;
  do { buf[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
  for (int k = n - 1; k >= 0; k--)
    out[(*i)++] = buf[k];
}

int fen_emit(const Position *pos, char *out, int outsz) {
  if (out == NULL || pos == NULL || outsz <= 0)
    return 0;
  if (!pos_validate(pos, NULL))
    return 0;   /* invalid input: do not write a partial or malformed FEN */
  int total = fen_len(pos);
  if (total + 1 > outsz)
    return 0;   /* buffer too small: leave *out untouched */
  int i = 0;

  for (int r = RANK_8; r >= RANK_1; r--) {
    int run = 0;
    for (int f = FILE_A; f < FILE_NB; f++) {
      Piece p = pos->mailbox[square_of((Square)f, (Square)r)];
      if (p == NO_PIECE) { run++; continue; }
      if (run) { out[i++] = (char)('0' + run); run = 0; }
      out[i++] = char_from_piece(p);
    }
    if (run) out[i++] = (char)('0' + run);
    if (r != RANK_1) out[i++] = '/';
  }

  out[i++] = ' ';
  out[i++] = (pos->side == WHITE) ? 'w' : 'b';

  out[i++] = ' ';
  if (pos->cr == 0) out[i++] = '-';
  else {
    if (pos->cr & CR_WK) out[i++] = 'K';
    if (pos->cr & CR_WQ) out[i++] = 'Q';
    if (pos->cr & CR_BK) out[i++] = 'k';
    if (pos->cr & CR_BQ) out[i++] = 'q';
  }

  out[i++] = ' ';
  if (pos->ep_sq >= SQ_NB) out[i++] = '-';
  else { out[i++] = (char)('a' + (pos->ep_sq & 7)); out[i++] = (char)('1' + (pos->ep_sq >> 3)); }

  out[i++] = ' ';
  write_num(out, &i, (int)pos->halfmove);
  out[i++] = ' ';
  write_num(out, &i, (int)pos->fullmove);

  out[i] = '\0';
  return 1;
}
