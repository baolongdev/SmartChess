#pragma once

#include <Arduino.h>

enum PieceType {
  PT_EMPTY = 0,
  PT_PAWN,
  PT_KNIGHT,
  PT_BISHOP,
  PT_ROOK,
  PT_QUEEN,
  PT_KING,
};

struct PieceInfo {
  String uid;
  char   piece;
  bool   captured;
};
