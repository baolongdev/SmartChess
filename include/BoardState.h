#pragma once

#include <Arduino.h>
#include "SmartChessConfig.h"

int squareIdx(int file, int rank);
String squareNameFromIdx(int idx);
bool isWhitePiece(char p);
void resetBoard(char board[8][8]);
void initStandardBoard(char board[8][8]);
void printBoard(const char board[8][8]);
