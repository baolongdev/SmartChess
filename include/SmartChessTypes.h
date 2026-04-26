#pragma once

#include <Arduino.h>

enum ScanState {
  SCAN_IDLE = 0,
  SCAN_LIFT_PENDING,
  SCAN_PIECE_LIFTED,
  SCAN_TRACKING_DESTINATION,
  SCAN_VERIFY,
  SCAN_FALLBACK,
};

enum MoveKind {
  MOVE_NORMAL = 0,
  MOVE_CAPTURE,
  MOVE_CASTLE_KINGSIDE,
  MOVE_CASTLE_QUEENSIDE,
  MOVE_EN_PASSANT,
  MOVE_PROMOTION,
};

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
  char piece;
  bool captured;
};
