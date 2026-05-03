#pragma once

#include "hardware/BoardConfig.h"
#include "app/AppTypes.h"

PieceType charToPieceType(char p);
bool isBlackPiece(char p);
bool isSameColor(char a, char b);

void generateCandidateSquares(const char board[8][8],
                              int fromFile,
                              int fromRank,
                              char piece,
                              int enPassantFile,
                              int enPassantRank,
                              bool whiteCanCastleK,
                              bool whiteCanCastleQ,
                              bool blackCanCastleK,
                              bool blackCanCastleQ,
                              int outList[MAX_CANDIDATES],
                              int &outCount);

