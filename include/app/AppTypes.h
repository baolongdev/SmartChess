#pragma once

// ---------------------------------------------------------------------------
// AppTypes.h — Application-level enumerations and data structures used
// across the SmartChess state machine.
// ---------------------------------------------------------------------------

#include <Arduino.h>

/** RFID scan / move-detection state machine states. */
enum ScanState {
  SCAN_IDLE = 0,
  SCAN_LIFT_PENDING,
  SCAN_PIECE_LIFTED,
  SCAN_TRACKING_DESTINATION,
  SCAN_VERIFY,
  SCAN_FALLBACK,
};

/** Classification of a completed chess move. */
enum MoveKind {
  MOVE_NORMAL = 0,
  MOVE_CAPTURE,
  MOVE_CASTLE_KINGSIDE,
  MOVE_CASTLE_QUEENSIDE,
  MOVE_EN_PASSANT,
  MOVE_PROMOTION,
};

/** Piece type codes (colour-independent). */
enum PieceType {
  PT_EMPTY = 0,
  PT_PAWN,
  PT_KNIGHT,
  PT_BISHOP,
  PT_ROOK,
  PT_QUEEN,
  PT_KING,
};

/** RFID tag record stored in the piece database. */
struct PieceInfo {
  String uid;
  char   piece;
  bool   captured;
};
