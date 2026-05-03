#include "chess/MoveGen.h"

#include "chess/BoardState.h"

PieceType charToPieceType(char p) {
  char lp = (char)tolower(p);
  if (lp == 'p') {
    return PT_PAWN;
  }
  if (lp == 'n') {
    return PT_KNIGHT;
  }
  if (lp == 'b') {
    return PT_BISHOP;
  }
  if (lp == 'r') {
    return PT_ROOK;
  }
  if (lp == 'q') {
    return PT_QUEEN;
  }
  if (lp == 'k') {
    return PT_KING;
  }
  return PT_EMPTY;
}

bool isBlackPiece(char p) {
  return p >= 'a' && p <= 'z';
}

bool isSameColor(char a, char b) {
  if (a == '.' || b == '.') {
    return false;
  }
  return (isWhitePiece(a) && isWhitePiece(b)) || (isBlackPiece(a) && isBlackPiece(b));
}

static bool inBoard(int f, int r) {
  return f >= 0 && f < 8 && r >= 0 && r < 8;
}

static void addCandidate(int f, int r, int outList[MAX_CANDIDATES], int &outCount) {
  if (outCount >= MAX_CANDIDATES) {
    return;
  }
  outList[outCount++] = squareIdx(f, r);
}

static char boardAt(const char board[8][8], int f, int r) {
  return board[f][r];
}

static void addCandidateIfValid(const char board[8][8],
                                int fromFile,
                                int fromRank,
                                int toFile,
                                int toRank,
                                char movingPiece,
                                int outList[MAX_CANDIDATES],
                                int &outCount) {
  (void)fromFile;
  (void)fromRank;
  if (!inBoard(toFile, toRank)) {
    return;
  }
  char dst = boardAt(board, toFile, toRank);
  if (isSameColor(movingPiece, dst)) {
    return;
  }
  addCandidate(toFile, toRank, outList, outCount);
}

static void genSliding(const char board[8][8],
                       int fromFile,
                       int fromRank,
                       char movingPiece,
                       int df,
                       int dr,
                       int outList[MAX_CANDIDATES],
                       int &outCount) {
  int f = fromFile + df;
  int r = fromRank + dr;
  while (inBoard(f, r)) {
    char dst = boardAt(board, f, r);
    if (dst == '.') {
      addCandidate(f, r, outList, outCount);
    } else {
      if (!isSameColor(movingPiece, dst)) {
        addCandidate(f, r, outList, outCount);
      }
      break;
    }
    f += df;
    r += dr;
  }
}

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
                              int &outCount) {
  outCount = 0;
  PieceType type = charToPieceType(piece);
  bool white = isWhitePiece(piece);

  if (type == PT_EMPTY) {
    return;
  }

  if (type == PT_PAWN) {
    int dir = white ? 1 : -1;
    int startRank = white ? 1 : 6;

    int oneStepRank = fromRank + dir;
    if (inBoard(fromFile, oneStepRank) && boardAt(board, fromFile, oneStepRank) == '.') {
      addCandidate(fromFile, oneStepRank, outList, outCount);

      int twoStepRank = fromRank + (2 * dir);
      if (fromRank == startRank && inBoard(fromFile, twoStepRank) && boardAt(board, fromFile, twoStepRank) == '.') {
        addCandidate(fromFile, twoStepRank, outList, outCount);
      }
    }

    int leftFile = fromFile - 1;
    int rightFile = fromFile + 1;

    if (inBoard(leftFile, oneStepRank)) {
      char dst = boardAt(board, leftFile, oneStepRank);
      if (dst != '.' && !isSameColor(piece, dst)) {
        addCandidate(leftFile, oneStepRank, outList, outCount);
      } else if (leftFile == enPassantFile && oneStepRank == enPassantRank) {
        addCandidate(leftFile, oneStepRank, outList, outCount);
      }
    }

    if (inBoard(rightFile, oneStepRank)) {
      char dst = boardAt(board, rightFile, oneStepRank);
      if (dst != '.' && !isSameColor(piece, dst)) {
        addCandidate(rightFile, oneStepRank, outList, outCount);
      } else if (rightFile == enPassantFile && oneStepRank == enPassantRank) {
        addCandidate(rightFile, oneStepRank, outList, outCount);
      }
    }
    return;
  }

  if (type == PT_KNIGHT) {
    const int deltas[8][2] = {
      {-2, -1}, {-2, 1}, {-1, -2}, {-1, 2}, {1, -2}, {1, 2}, {2, -1}, {2, 1},
    };
    for (int i = 0; i < 8; i++) {
      addCandidateIfValid(board,
                          fromFile,
                          fromRank,
                          fromFile + deltas[i][0],
                          fromRank + deltas[i][1],
                          piece,
                          outList,
                          outCount);
    }
    return;
  }

  if (type == PT_BISHOP || type == PT_QUEEN) {
    genSliding(board, fromFile, fromRank, piece, 1, 1, outList, outCount);
    genSliding(board, fromFile, fromRank, piece, 1, -1, outList, outCount);
    genSliding(board, fromFile, fromRank, piece, -1, 1, outList, outCount);
    genSliding(board, fromFile, fromRank, piece, -1, -1, outList, outCount);
  }

  if (type == PT_ROOK || type == PT_QUEEN) {
    genSliding(board, fromFile, fromRank, piece, 1, 0, outList, outCount);
    genSliding(board, fromFile, fromRank, piece, -1, 0, outList, outCount);
    genSliding(board, fromFile, fromRank, piece, 0, 1, outList, outCount);
    genSliding(board, fromFile, fromRank, piece, 0, -1, outList, outCount);
  }

  if (type == PT_KING) {
    for (int df = -1; df <= 1; df++) {
      for (int dr = -1; dr <= 1; dr++) {
        if (df == 0 && dr == 0) {
          continue;
        }
        addCandidateIfValid(board,
                            fromFile,
                            fromRank,
                            fromFile + df,
                            fromRank + dr,
                            piece,
                            outList,
                            outCount);
      }
    }

    if (white && fromFile == 4 && fromRank == 0) {
      if (whiteCanCastleK && boardAt(board, 5, 0) == '.' && boardAt(board, 6, 0) == '.') {
        addCandidate(6, 0, outList, outCount);
      }
      if (whiteCanCastleQ && boardAt(board, 3, 0) == '.' && boardAt(board, 2, 0) == '.' && boardAt(board, 1, 0) == '.') {
        addCandidate(2, 0, outList, outCount);
      }
    }
    if (!white && fromFile == 4 && fromRank == 7) {
      if (blackCanCastleK && boardAt(board, 5, 7) == '.' && boardAt(board, 6, 7) == '.') {
        addCandidate(6, 7, outList, outCount);
      }
      if (blackCanCastleQ && boardAt(board, 3, 7) == '.' && boardAt(board, 2, 7) == '.' && boardAt(board, 1, 7) == '.') {
        addCandidate(2, 7, outList, outCount);
      }
    }
  }
}

