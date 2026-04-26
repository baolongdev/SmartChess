#include "Fen.h"

String boardToFenPlacement(const char board[8][8]) {
  String fen = "";
  for (int rank = 7; rank >= 0; rank--) {
    int emptyCount = 0;
    for (int file = 0; file < 8; file++) {
      char p = board[file][rank];
      if (p == '.') {
        emptyCount++;
      } else {
        if (emptyCount > 0) {
          fen += String(emptyCount);
          emptyCount = 0;
        }
        fen += p;
      }
    }
    if (emptyCount > 0) {
      fen += String(emptyCount);
    }
    if (rank > 0) {
      fen += '/';
    }
  }
  return fen;
}

String castlingRightsFen(const char board[8][8],
                         bool whiteKingMoved,
                         bool blackKingMoved,
                         bool whiteRookAMoved,
                         bool whiteRookHMoved,
                         bool blackRookAMoved,
                         bool blackRookHMoved) {
  String rights = "";
  if (!whiteKingMoved && board[4][0] == 'K') {
    if (!whiteRookHMoved && board[7][0] == 'R') {
      rights += 'K';
    }
    if (!whiteRookAMoved && board[0][0] == 'R') {
      rights += 'Q';
    }
  }
  if (!blackKingMoved && board[4][7] == 'k') {
    if (!blackRookHMoved && board[7][7] == 'r') {
      rights += 'k';
    }
    if (!blackRookAMoved && board[0][7] == 'r') {
      rights += 'q';
    }
  }
  if (rights.length() == 0) {
    rights = "-";
  }
  return rights;
}

String currentFEN(const char board[8][8],
                  bool whiteTurn,
                  int enPassantFile,
                  int enPassantRank,
                  int halfmoveClock,
                  int fullmoveNumber,
                  bool whiteKingMoved,
                  bool blackKingMoved,
                  bool whiteRookAMoved,
                  bool whiteRookHMoved,
                  bool blackRookAMoved,
                  bool blackRookHMoved) {
  String fen = boardToFenPlacement(board);
  fen += ' ';
  fen += whiteTurn ? 'w' : 'b';
  fen += ' ';
  fen += castlingRightsFen(board,
                           whiteKingMoved,
                           blackKingMoved,
                           whiteRookAMoved,
                           whiteRookHMoved,
                           blackRookAMoved,
                           blackRookHMoved);
  fen += ' ';

  if (enPassantFile >= 0 && enPassantRank >= 0) {
    fen += (char)('a' + enPassantFile);
    fen += (char)('1' + enPassantRank);
  } else {
    fen += '-';
  }

  fen += ' ';
  fen += String(halfmoveClock);
  fen += ' ';
  fen += String(fullmoveNumber);
  return fen;
}
