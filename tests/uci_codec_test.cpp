/* QwenChess: the UCI move codec (focused unit tests).
 *
 * Exercises the single production move<->text codec: all four orthodox castles (the
 * wire king g/c destination, never the internal rook origin), all four promotions +
 * an underpromotion, an en-passant capture, a pinned/illegal move (rejected), 0000 and
 * malformed tokens (rejected), and emit->parse->emit round trips. The legal set used
 * for resolution comes from movegen_legal, so a pinned move is rejected because it is
 * not legal, not because of a separate legality routine.
 */
#include <cstdio>
#include <cstring>

#include "core/square.hpp"
#include "engine/uci_codec.hpp"
#include "position/fen.hpp"
#include "position/position.hpp"

#include "test_util.hpp"

using namespace qwc;

static Position g_pos;   /* the current position the codec resolves against */

/* Parse a token in g_pos into m; returns the codec result. */
static int pparse(const char *tok, Move& m) {
  return uci_move_parse(g_pos, tok, m) ? 1 : 0;
}
/* Emit m into a caller buffer of the documented capacity; returns the text length. */
static int pemit(Move m, char* buf) {
  return uci_move_emit(m, buf, kUciMoveText);
}

int main() {
  qwc_begin("uci_codec");
  g_pos = Position{};
  char buf[kUciMoveText];
  Move m = move_none();

  /* ---- all four orthodox castles ------------------------------------------------ */
  struct CastleCase { const char *fen; const char *wire; Square int_to; };
  const CastleCase castles[] = {
    { "4k3/8/8/8/8/8/8/4K2R w K - 0 1",  "e1g1", square_of(7, 0) },   /* w kingside -> h1 */
    { "4k3/8/8/8/8/8/8/R3K2R w Q - 0 1", "e1c1", square_of(0, 0) },   /* w queenside -> a1 */
    { "4k2r/8/8/8/8/8/8/4K3 b k - 0 1",  "e8g8", square_of(7, 7) },      /* b kingside -> h8 */
    { "r3k2r/8/8/8/8/8/8/4K3 b q - 0 1", "e8c8", square_of(0, 7) },/* b queenside -> a8 */
  };
  for (const auto& c : castles) {
    QWC_TRUE(fen_load(g_pos, c.fen), "castle FEN loads");
    QWC_TRUE(pparse(c.wire, m) == 1, "castle parses");
    QWC_TRUE(is_castle(m), "parsed move is a castle");
    QWC_EQ_I(move_to(m), c.int_to, "internal castle 'to' is the rook origin");
    int n = pemit(m, buf);
    QWC_EQ_I(n, 4, "castle emits 4 chars");
    QWC_TRUE(memcmp(buf, c.wire, 5) == 0, "castle emit == wire king g/c (no rook-origin leak)");
    /* round trip: parse the emitted text again in the same position */
    Move m2 = move_none();
    QWC_TRUE(pparse(buf, m2) == 1 && m2 == m, "castle emit->parse round trip");
  }

  /* ---- promotions (white Q/R/B/N + a black underpromotion) ----------------------- */
  const char *promof = "8/P7/8/8/8/8/8/4K2k w - - 0 1";   /* white pawn a7 -> a8 */
  QWC_TRUE(fen_load(g_pos, promof), "promo FEN loads");
  const char *ptok[] = { "a7a8q", "a7a8r", "a7a8b", "a7a8n" };
  const u32 ptype[] = { to_under(PieceType::QUEEN), to_under(PieceType::ROOK),
                        to_under(PieceType::BISHOP), to_under(PieceType::KNIGHT) };
  const char *pchr[] = { "a7a8q", "a7a8r", "a7a8b", "a7a8n" };
  for (int i = 0; i < 4; ++i) {
    QWC_TRUE(pparse(ptok[i], m) == 1, "promotion parses");
    QWC_TRUE(is_promotion(m) && move_promo(m) == ptype[i], "promo flag + type");
    QWC_EQ_I(move_from(m), square_of(0, 6), "promo from a7");
    QWC_EQ_I(move_to(m), square_of(0, 7), "promo to a8");
    int n = pemit(m, buf);
    QWC_EQ_I(n, 5, "promo emits 5 chars");
    QWC_TRUE(memcmp(buf, pchr[i], 6) == 0, "promo emit == token");
  }
  /* a black underpromotion (a2 -> a1=n) */
  const char *underf = "4K2k/8/8/8/8/8/p7/8 b - - 0 1";
  QWC_TRUE(fen_load(g_pos, underf), "underpromo FEN loads");
  QWC_TRUE(pparse("a2a1n", m) == 1, "underpromotion parses");
  QWC_TRUE(is_promotion(m) && move_promo(m) == to_under(PieceType::KNIGHT), "underpromo is =n");
  pemit(m, buf);
  QWC_TRUE(memcmp(buf, "a2a1n", 6) == 0, "underpromo emit == token");

  /* ---- en passant ---------------------------------------------------------------- */
  const char *epf = "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1";
  QWC_TRUE(fen_load(g_pos, epf), "ep FEN loads");
  QWC_TRUE(pparse("e5d6", m) == 1, "ep parses");
  QWC_TRUE(is_ep(m), "ep flag set");
  QWC_EQ_I(move_from(m), square_of(4, 4), "ep from e5");
  QWC_EQ_I(move_to(m), square_of(3, 5), "ep to the recorded target d6");
  pemit(m, buf);
  QWC_TRUE(memcmp(buf, "e5d6", 5) == 0, "ep emit == token");

  /* ---- a pinned / illegal move is rejected (not in the legal set) ---------------- */
  /* black rook e8 pins the white bishop e5 to the white king e1; the bishop cannot
   * leave the e-file, so a diagonal move is pseudo-legal but NOT legal. */
  const char *pinf = "4rk2/8/8/4B3/8/8/8/4K3 w - - 0 1";
  QWC_TRUE(fen_load(g_pos, pinf), "pinned FEN loads");
  Move sentinel = move_none();
  QWC_TRUE(pparse("e5b2", sentinel) == 0, "pinned bishop move rejected");
  QWC_EQ_I(sentinel, move_none(), "rejected parse leaves `out` unchanged");

  /* ---- 0000 and malformed tokens are rejected ------------------------------------- */
  QWC_TRUE(fen_load(g_pos, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"), "startpos loads");
  const char *bad[] = { "0000", "e2", "e2e44", "e2e4x", "a1a1", "i1a1", "e2e9", "" };
  for (const char* b : bad) {
    Move s = move_none();
    QWC_TRUE(pparse(b, s) == 0, "malformed token rejected");
    QWC_EQ_I(s, move_none(), "malformed parse leaves `out` unchanged");
  }

  /* ---- a quiet move round trips and matches a hand-built encoding ----------------- */
  QWC_TRUE(pparse("e2e4", m) == 1, "quiet e2e4 parses");
  QWC_EQ_I(m, make_move(square_of(4, 1), square_of(4, 3), MoveFlag::NO_FLAGS, 0), "quiet == hand-built");
  pemit(m, buf);
  QWC_TRUE(memcmp(buf, "e2e4", 5) == 0, "quiet emit == token");
  QWC_TRUE(move_repr_is_valid(m), "emitted move has a valid representation");

  int failed = qwc_end();
  return failed ? 1 : 0;
}
