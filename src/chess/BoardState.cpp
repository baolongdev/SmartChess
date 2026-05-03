#include "chess/BoardState.h"

int squareIdx(int file, int rank) {
  return file * 8 + rank;
}

String squareNameFromIdx(int idx) {
  String s = "";
  s += (char)('a' + idx / 8);
  s += (char)('1' + idx % 8);
  return s;
}

bool isWhitePiece(char p) {
  return p >= 'A' && p <= 'Z' && p != '.';
}

void resetBoard(char board[8][8]) {
  for (int f = 0; f < 8; f++) {
    for (int r = 0; r < 8; r++) {
      board[f][r] = '.';
    }
  }
}

void initStandardBoard(char board[8][8]) {
  resetBoard(board);

  board[0][0] = 'R';
  board[1][0] = 'N';
  board[2][0] = 'B';
  board[3][0] = 'Q';
  board[4][0] = 'K';
  board[5][0] = 'B';
  board[6][0] = 'N';
  board[7][0] = 'R';
  for (int f = 0; f < 8; f++) {
    board[f][1] = 'P';
  }

  board[0][7] = 'r';
  board[1][7] = 'n';
  board[2][7] = 'b';
  board[3][7] = 'q';
  board[4][7] = 'k';
  board[5][7] = 'b';
  board[6][7] = 'n';
  board[7][7] = 'r';
  for (int f = 0; f < 8; f++) {
    board[f][6] = 'p';
  }
}

void printBoard(const char board[8][8]) {
  // Buffer the entire board into one String then print atomically.
  // Multiple Serial.print() calls interleave with BLE callbacks (different
  // FreeRTOS task) and corrupt the output.
  String out;
  out.reserve(420);
  out += "\n  +---+---+---+---+---+---+---+---+\n";
  for (int rank = 7; rank >= 0; rank--) {
    out += (char)('1' + rank);
    out += " |";
    for (int file = 0; file < 8; file++) {
      char p = board[file][rank];
      out += ' ';
      out += (p == '.' ? ' ' : p);
      out += " |";
    }
    out += "\n  +---+---+---+---+---+---+---+---+\n";
  }
  out += "    a   b   c   d   e   f   g   h\n";
  Serial.print(out);
}

