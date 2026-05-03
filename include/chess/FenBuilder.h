#pragma once

// ---------------------------------------------------------------------------
// FenBuilder.h — Utilities to build FEN strings from the board state.
// ---------------------------------------------------------------------------

#include <Arduino.h>

/** Build the piece-placement section of a FEN string (rank 8 first). */
String fenBoardToPlacement(const char board[8][8]);

/** Build the castling-rights token ("KQkq", partial, or "-"). */
String fenCastlingRights(const char board[8][8],
                         bool whiteKingMoved, bool blackKingMoved,
                         bool whiteRookAMoved, bool whiteRookHMoved,
                         bool blackRookAMoved, bool blackRookHMoved);

/** Build and return the full FEN string as a String. */
String fenBuild(const char board[8][8],
                bool whiteTurn, int enPassantFile, int enPassantRank,
                int halfmoveClock, int fullmoveNumber,
                bool whiteKingMoved, bool blackKingMoved,
                bool whiteRookAMoved, bool whiteRookHMoved,
                bool blackRookAMoved, bool blackRookHMoved);

/** Build the full FEN string into a caller-supplied buffer. */
void fenBuild(const char board[8][8],
              char *out, size_t len,
              bool whiteTurn, int enPassantFile, int enPassantRank,
              int halfmoveClock, int fullmoveNumber,
              bool whiteKingMoved, bool blackKingMoved,
              bool whiteRookAMoved, bool whiteRookHMoved,
              bool blackRookAMoved, bool blackRookHMoved);
