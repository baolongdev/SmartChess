#pragma once

#include <Arduino.h>

String boardToFenPlacement(const char board[8][8]);
String castlingRightsFen(const char board[8][8],
                         bool whiteKingMoved,
                         bool blackKingMoved,
                         bool whiteRookAMoved,
                         bool whiteRookHMoved,
                         bool blackRookAMoved,
                         bool blackRookHMoved);
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
                  bool blackRookHMoved);
