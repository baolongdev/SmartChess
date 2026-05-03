#include "chess/FenBuilder.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static bool appendFmt(char *out, size_t len, size_t &pos, const char *fmt, ...) {
  if (!out || len == 0 || !fmt) return false;
  if (pos >= len - 1) { out[len - 1] = '\0'; return false; }

  va_list args;
  va_start(args, fmt);
  int wrote = vsnprintf(out + pos, len - pos, fmt, args);
  va_end(args);

  if (wrote < 0) { out[len - 1] = '\0'; return false; }

  size_t available = len - pos;
  if ((size_t)wrote >= available) { pos = len - 1; out[len - 1] = '\0'; return false; }

  pos += (size_t)wrote;
  return true;
}

static void buildPlacementBuf(const char board[8][8], char *out, size_t len) {
  if (!out || len == 0) return;
  size_t pos = 0;
  out[0] = '\0';
  for (int rank = 7; rank >= 0 && pos < len - 1; rank--) {
    int empty = 0;
    for (int file = 0; file < 8 && pos < len - 1; file++) {
      char p = board[file][rank];
      if (p == '.') {
        empty++;
      } else {
        if (empty > 0) { appendFmt(out, len, pos, "%d", empty); empty = 0; }
        if (pos < len - 1) out[pos++] = p;
      }
    }
    if (empty > 0 && pos < len - 1) appendFmt(out, len, pos, "%d", empty);
    if (rank > 0 && pos < len - 1) out[pos++] = '/';
  }
  out[pos] = '\0';
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

String fenBoardToPlacement(const char board[8][8]) {
  char buf[80];
  buildPlacementBuf(board, buf, sizeof(buf));
  return String(buf);
}

String fenCastlingRights(const char board[8][8],
                          bool whiteKingMoved, bool blackKingMoved,
                          bool whiteRookAMoved, bool whiteRookHMoved,
                          bool blackRookAMoved, bool blackRookHMoved) {
  String rights = "";
  if (!whiteKingMoved && board[4][0] == 'K') {
    if (!whiteRookHMoved && board[7][0] == 'R') rights += 'K';
    if (!whiteRookAMoved && board[0][0] == 'R') rights += 'Q';
  }
  if (!blackKingMoved && board[4][7] == 'k') {
    if (!blackRookHMoved && board[7][7] == 'r') rights += 'k';
    if (!blackRookAMoved && board[0][7] == 'r') rights += 'q';
  }
  return rights.length() > 0 ? rights : "-";
}

String fenBuild(const char board[8][8],
                bool whiteTurn, int enPassantFile, int enPassantRank,
                int halfmoveClock, int fullmoveNumber,
                bool whiteKingMoved, bool blackKingMoved,
                bool whiteRookAMoved, bool whiteRookHMoved,
                bool blackRookAMoved, bool blackRookHMoved) {
  char out[128];
  fenBuild(board, out, sizeof(out), whiteTurn,
           enPassantFile, enPassantRank, halfmoveClock, fullmoveNumber,
           whiteKingMoved, blackKingMoved, whiteRookAMoved, whiteRookHMoved,
           blackRookAMoved, blackRookHMoved);
  return String(out);
}

void fenBuild(const char board[8][8],
              char *out, size_t len,
              bool whiteTurn, int enPassantFile, int enPassantRank,
              int halfmoveClock, int fullmoveNumber,
              bool whiteKingMoved, bool blackKingMoved,
              bool whiteRookAMoved, bool whiteRookHMoved,
              bool blackRookAMoved, bool blackRookHMoved) {
  if (!out || len == 0) return;

  size_t pos = 0;
  buildPlacementBuf(board, out, len);
  pos = strlen(out);

  appendFmt(out, len, pos, " %c ", whiteTurn ? 'w' : 'b');

  bool hasCastling = false;
  if (!whiteKingMoved && board[4][0] == 'K') {
    if (!whiteRookHMoved && board[7][0] == 'R') hasCastling = appendFmt(out, len, pos, "K") || hasCastling;
    if (!whiteRookAMoved && board[0][0] == 'R') hasCastling = appendFmt(out, len, pos, "Q") || hasCastling;
  }
  if (!blackKingMoved && board[4][7] == 'k') {
    if (!blackRookHMoved && board[7][7] == 'r') hasCastling = appendFmt(out, len, pos, "k") || hasCastling;
    if (!blackRookAMoved && board[0][7] == 'r') hasCastling = appendFmt(out, len, pos, "q") || hasCastling;
  }
  if (!hasCastling) appendFmt(out, len, pos, "-");

  appendFmt(out, len, pos, " ");
  if (enPassantFile >= 0 && enPassantRank >= 0) {
    appendFmt(out, len, pos, "%c%c", 'a' + enPassantFile, '1' + enPassantRank);
  } else {
    appendFmt(out, len, pos, "-");
  }

  appendFmt(out, len, pos, " %d %d", halfmoveClock, fullmoveNumber);
}
