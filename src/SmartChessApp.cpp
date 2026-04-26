#include "SmartChessApp.h"

#include <Arduino.h>
#include <MFRC522.h>

#include "BoardState.h"
#include "BleFen.h"
#include "Fen.h"
#include "MoveGen.h"
#include "RfidScanner.h"
#include "ScanDebug.h"
#include "SmartChessConfig.h"
#include "SmartChessTypes.h"

static MFRC522 mfrc522(SS_PIN, RST_PIN);

static char board[8][8];
static String squareUID[NUM_ANTENNAS];
static String scanBuf[NUM_ANTENNAS];
static PieceInfo pieceDB[MAX_PIECES];

static int pieceDBCount = 0;
static bool gameStarted = false;

static bool whiteTurn = true;
static int fullmoveNumber = 1;
static int halfmoveClock = 0;
static int enPassantFile = -1;
static int enPassantRank = -1;

static bool whiteKingMoved = false;
static bool blackKingMoved = false;
static bool whiteRookAMoved = false;
static bool whiteRookHMoved = false;
static bool blackRookAMoved = false;
static bool blackRookHMoved = false;

static int occupiedList[NUM_ANTENNAS];
static int occupiedCount = 0;

static ScanState scanState = SCAN_IDLE;
static int liftedFromIdx = -1;
static int liftedToIdx = -1;
static String liftedUID = "";
static char liftedPiece = '.';
static unsigned long liftDetectedMs = 0;
static unsigned long trackingStartMs = 0;
static unsigned long fallbackStartMs = 0;
static unsigned long lastFallbackTryMs = 0;
static int verifyCount = 0;
static int candidateList[MAX_CANDIDATES];
static int candidateCount = 0;
static String pendingLiftUID = "";
static int pendingLiftIdx = -1;
static unsigned long pendingLiftFirstMs = 0;

static bool scanVerbose = false;
static bool scanContinuous = true;
static bool scanUsePieceLetters = true;
static unsigned long lastScanPrintMs = 0;
static unsigned long lastTimingPrintMs = 0;
static uint16_t lastScannedCount = 0;
static uint32_t lastFullScanUs = 0;
static uint32_t lastAvgCellUs = 0;
static uint32_t lastMinCellUs = 0;
static uint32_t lastMaxCellUs = 0;

static uint16_t cycleScanCount = 0;
static uint32_t cycleScanStartUs = 0;
static uint32_t cycleScanSumUs = 0;
static uint32_t cycleScanMinUs = 0;
static uint32_t cycleScanMaxUs = 0;

static String lastFenSent = "";
static unsigned long lastBleKeepAliveMs = 0;
constexpr unsigned long BLE_KEEPALIVE_MS = 1500;

static unsigned long gameStartTimeMs = 0;
static int totalMovesCount = 0;

static String cmdBuffer = "";
static unsigned long cmdLastByteMs = 0;
constexpr unsigned long CMD_COMMIT_IDLE_MS = 120;
static String startFailReason = "UNKNOWN";
static String startFailDetail = "";

static void rebuildOccupiedListFromBoard() {
  occupiedCount = 0;
  for (int file = 0; file < 8; file++) {
    for (int rank = 0; rank < 8; rank++) {
      if (board[file][rank] != '.') {
        occupiedList[occupiedCount++] = squareIdx(file, rank);
      }
    }
  }
}

static void sanitizeSquareUIDByBoard() {
  for (int i = 0; i < NUM_ANTENNAS; i++) {
    int file = i / 8;
    int rank = i % 8;
    if (board[file][rank] == '.') {
      squareUID[i] = "";
    }
  }
}

static char lookupUID(const String &uid) {
  for (int i = 0; i < pieceDBCount; i++) {
    if (pieceDB[i].uid == uid) {
      return pieceDB[i].piece;
    }
  }
  return '?';
}

static void registerPiece(const String &uid, char piece) {
  for (int i = 0; i < pieceDBCount; i++) {
    if (pieceDB[i].uid == uid) {
      pieceDB[i].piece = piece;
      pieceDB[i].captured = false;
      return;
    }
  }

  if (pieceDBCount < MAX_PIECES) {
    pieceDB[pieceDBCount].uid = uid;
    pieceDB[pieceDBCount].piece = piece;
    pieceDB[pieceDBCount].captured = false;
    pieceDBCount++;
  }
}

static void markCaptured(const String &uid) {
  if (uid.length() == 0) {
    return;
  }
  for (int i = 0; i < pieceDBCount; i++) {
    if (pieceDB[i].uid == uid) {
      pieceDB[i].captured = true;
      return;
    }
  }
}

static String buildCurrentFen() {
  return currentFEN(board,
                    whiteTurn,
                    enPassantFile,
                    enPassantRank,
                    halfmoveClock,
                    fullmoveNumber,
                    whiteKingMoved,
                    blackKingMoved,
                    whiteRookAMoved,
                    whiteRookHMoved,
                    blackRookAMoved,
                    blackRookHMoved);
}

static void publishFenIfChanged(const String &fen) {
  if (fen == lastFenSent) {
    return;
  }

  bleFenPublish(fen);
  lastFenSent = fen;
  if (bleFenIsConnected()) {
    Serial.println(F("[BLE] FEN notify sent"));
  }
}

static void printFEN() {
  String fen = buildCurrentFen();
  Serial.print(F("[FEN] "));
  Serial.println(fen);
  publishFenIfChanged(fen);
}

static void resetTrackingState() {
  scanState = SCAN_IDLE;
  liftedFromIdx = -1;
  liftedToIdx = -1;
  liftedUID = "";
  liftedPiece = '.';
  liftDetectedMs = 0;
  trackingStartMs = 0;
  fallbackStartMs = 0;
  lastFallbackTryMs = 0;
  verifyCount = 0;
  candidateCount = 0;
  pendingLiftUID = "";
  pendingLiftIdx = -1;
  pendingLiftFirstMs = 0;
}

static void setPendingLift(int idx, const String &uid) {
  pendingLiftIdx = idx;
  pendingLiftUID = uid;
  pendingLiftFirstMs = millis();
  scanState = SCAN_LIFT_PENDING;
}

static void clearPendingLift() {
  pendingLiftUID = "";
  pendingLiftIdx = -1;
  pendingLiftFirstMs = 0;
}

static void abortTrackingRestoreSource() {
  if (liftedFromIdx >= 0 && liftedUID.length() > 0 && squareUID[liftedFromIdx].length() == 0) {
    squareUID[liftedFromIdx] = liftedUID;
  }
  resetTrackingState();
}

static void resetGameRuntimeState() {
  whiteTurn = true;
  fullmoveNumber = 1;
  halfmoveClock = 0;
  enPassantFile = -1;
  enPassantRank = -1;

  whiteKingMoved = false;
  blackKingMoved = false;
  whiteRookAMoved = false;
  whiteRookHMoved = false;
  blackRookAMoved = false;
  blackRookHMoved = false;

  pieceDBCount = 0;
  for (int i = 0; i < NUM_ANTENNAS; i++) {
    squareUID[i] = "";
    scanBuf[i] = "";
  }

  occupiedCount = 0;
  lastFenSent = "";
  lastBleKeepAliveMs = 0;
  resetTrackingState();
}

static void prepareNewGameModel() {
  resetGameRuntimeState();
  initStandardBoard(board);
  rebuildOccupiedListFromBoard();
}

static bool validateNoDuplicateUids(const bool startMask[NUM_ANTENNAS]) {
  for (int i = 0; i < NUM_ANTENNAS; i++) {
    if (!startMask[i] || scanBuf[i].length() == 0) {
      continue;
    }
    for (int j = i + 1; j < NUM_ANTENNAS; j++) {
      if (!startMask[j] || scanBuf[j].length() == 0) {
        continue;
      }
      if (scanBuf[i] == scanBuf[j]) {
        Serial.print(F("[START] UID trung lap: "));
        Serial.print(scanBuf[i]);
        Serial.print(F(" tai "));
        Serial.print(squareNameFromIdx(i));
        Serial.print(F(" va "));
        Serial.println(squareNameFromIdx(j));
        startFailDetail = String("uid=") + scanBuf[i] + String("@") + squareNameFromIdx(i) + String(",") + squareNameFromIdx(j);
        return false;
      }
    }
  }
  return true;
}

static bool startAndLearnInitial32() {
  prepareNewGameModel();
  startFailReason = "UNKNOWN";
  startFailDetail = "";

  bool startMask[NUM_ANTENNAS] = {false};
  for (int file = 0; file < 8; file++) {
    for (int rank = 0; rank < 8; rank++) {
      if (board[file][rank] != '.') {
        startMask[squareIdx(file, rank)] = true;
      }
    }
  }

  Serial.println(F("[START] Kiem tra 32 o xuat phat..."));
  selectiveScanToBuffer(mfrc522,
                        startMask,
                        scanBuf,
                        lastScannedCount,
                        lastFullScanUs,
                        lastAvgCellUs,
                        lastMinCellUs,
                        lastMaxCellUs);

  int foundCount = 0;
  for (int i = 0; i < NUM_ANTENNAS; i++) {
    if (!startMask[i]) {
      continue;
    }
    if (scanBuf[i].length() > 0) {
      foundCount++;
    }
  }

  if (foundCount < 32) {
    String missingSquares = "";
    Serial.print(F("[START] Thieu quan: "));
    Serial.print(foundCount);
    Serial.println(F("/32"));
    Serial.println(F("[START] Cac o chua co UID:"));
    for (int i = 0; i < NUM_ANTENNAS; i++) {
      if (!startMask[i]) {
        continue;
      }
      if (scanBuf[i].length() == 0) {
        Serial.print(F("  - "));
        Serial.println(squareNameFromIdx(i));
        if (missingSquares.length() > 0) {
          missingSquares += ",";
        }
        missingSquares += squareNameFromIdx(i);
      }
    }
    gameStarted = false;
    startFailReason = "MISSING_PIECES";
    startFailDetail = missingSquares;
    bleFenLog(String("[ERR] MISSING_SQUARES ") + missingSquares + String(" | place piece and START again"));
    return false;
  }

  if (!validateNoDuplicateUids(startMask)) {
    gameStarted = false;
    startFailReason = "DUPLICATE_UID";
    if (startFailDetail.length() == 0) {
      startFailDetail = "unknown";
    }
    bleFenLog(String("[ERR] DUPLICATE_UID ") + startFailDetail + String(" | fix tags and START again"));
    return false;
  }

  for (int i = 0; i < NUM_ANTENNAS; i++) {
    if (!startMask[i]) {
      squareUID[i] = "";
      continue;
    }
    squareUID[i] = scanBuf[i];
    int file = i / 8;
    int rank = i % 8;
    registerPiece(squareUID[i], board[file][rank]);
  }

  rebuildOccupiedListFromBoard();
  sanitizeSquareUIDByBoard();
  resetTrackingState();
  gameStarted = true;
  startFailDetail = "";
  gameStartTimeMs = millis();
  totalMovesCount = 0;

  Serial.println(F("[START] Du 32 quan. Bat dau tracking."));
  bleFenLog(String("[READY] STARTED | pieces=32"));
  printBoard(board);
  printFEN();
  return true;
}

static void beginCycleScanMetrics() {
  cycleScanCount = 0;
  cycleScanStartUs = micros();
  cycleScanSumUs = 0;
  cycleScanMinUs = 0xFFFFFFFF;
  cycleScanMaxUs = 0;
}

static void endCycleScanMetrics() {
  lastScannedCount = cycleScanCount;
  lastFullScanUs = micros() - cycleScanStartUs;

  if (cycleScanCount == 0) {
    lastAvgCellUs = 0;
    lastMinCellUs = 0;
    lastMaxCellUs = 0;
    return;
  }

  lastAvgCellUs = cycleScanSumUs / cycleScanCount;
  lastMinCellUs = cycleScanMinUs;
  lastMaxCellUs = cycleScanMaxUs;
}

static String timedScanSquare(int idx) {
  uint32_t st = micros();
  String uid = scanUID(mfrc522, idx);
  uint32_t dt = micros() - st;

  cycleScanCount++;
  cycleScanSumUs += dt;
  if (dt < cycleScanMinUs) {
    cycleScanMinUs = dt;
  }
  if (dt > cycleScanMaxUs) {
    cycleScanMaxUs = dt;
  }
  return uid;
}

static void markCapturedRookRights(char capturedPiece, int toFile, int toRank) {
  if (capturedPiece == 'R') {
    if (toFile == 0 && toRank == 0) {
      whiteRookAMoved = true;
    }
    if (toFile == 7 && toRank == 0) {
      whiteRookHMoved = true;
    }
  }
  if (capturedPiece == 'r') {
    if (toFile == 0 && toRank == 7) {
      blackRookAMoved = true;
    }
    if (toFile == 7 && toRank == 7) {
      blackRookHMoved = true;
    }
  }
}

static void moveRookForCastling(bool white, bool kingSide) {
  if (white) {
    if (kingSide) {
      int rookFrom = squareIdx(7, 0);
      int rookTo = squareIdx(5, 0);
      board[5][0] = 'R';
      board[7][0] = '.';
      squareUID[rookTo] = squareUID[rookFrom];
      squareUID[rookFrom] = "";
      whiteRookHMoved = true;
    } else {
      int rookFrom = squareIdx(0, 0);
      int rookTo = squareIdx(3, 0);
      board[3][0] = 'R';
      board[0][0] = '.';
      squareUID[rookTo] = squareUID[rookFrom];
      squareUID[rookFrom] = "";
      whiteRookAMoved = true;
    }
  } else {
    if (kingSide) {
      int rookFrom = squareIdx(7, 7);
      int rookTo = squareIdx(5, 7);
      board[5][7] = 'r';
      board[7][7] = '.';
      squareUID[rookTo] = squareUID[rookFrom];
      squareUID[rookFrom] = "";
      blackRookHMoved = true;
    } else {
      int rookFrom = squareIdx(0, 7);
      int rookTo = squareIdx(3, 7);
      board[3][7] = 'r';
      board[0][7] = '.';
      squareUID[rookTo] = squareUID[rookFrom];
      squareUID[rookFrom] = "";
      blackRookAMoved = true;
    }
  }
}

static const char *sideName(bool white) {
  return white ? "WHITE" : "BLACK";
}

static bool applyMove(int fromIdx, int toIdx, const String &movedUid) {
  int fromFile = fromIdx / 8;
  int fromRank = fromIdx % 8;
  int toFile = toIdx / 8;
  int toRank = toIdx % 8;

  char piece = board[fromFile][fromRank];
  if (piece == '.') {
    piece = lookupUID(movedUid);
    if (piece == '?') {
      Serial.println(F("[WARN] Khong xac dinh duoc quan da di."));
      bleFenLog(String("[ERR] UNKNOWN_PIECE uid=") + movedUid + String(" | reset piece then retry"));
      return false;
    }
  }

  bool movingWhite = isWhitePiece(piece);
  if ((whiteTurn && !movingWhite) || (!whiteTurn && movingWhite)) {
    Serial.println(F("[TURN] Wrong side moved."));
    bleFenLog(String("[ERR] WRONG_TURN expected=") + sideName(whiteTurn) +
              String(" got=") + sideName(movingWhite) +
              String(" at ") + squareNameFromIdx(fromIdx) +
              String(" | place piece back"));
    return false;
  }

  char capturedPiece = board[toFile][toRank];
  bool isCapture = (capturedPiece != '.');
  if (isCapture) {
    markCaptured(squareUID[toIdx]);
  }
  markCapturedRookRights(capturedPiece, toFile, toRank);

  bool castleKingSide = false;
  bool castleQueenSide = false;
  bool enPassantCapture = false;
  bool promoted = false;

  if ((piece == 'K' || piece == 'k') && abs(toFile - fromFile) == 2 && fromRank == toRank) {
    castleKingSide = (toFile > fromFile);
    castleQueenSide = !castleKingSide;
  }

  if ((piece == 'P' || piece == 'p') && fromFile != toFile && capturedPiece == '.') {
    int capRank = (piece == 'P') ? (toRank - 1) : (toRank + 1);
    int capIdx = squareIdx(toFile, capRank);
    markCaptured(squareUID[capIdx]);
    board[toFile][capRank] = '.';
    squareUID[capIdx] = "";
    enPassantCapture = true;
    isCapture = true;
  }

  board[toFile][toRank] = piece;
  board[fromFile][fromRank] = '.';
  squareUID[toIdx] = movedUid;
  squareUID[fromIdx] = "";

  if (castleKingSide) {
    moveRookForCastling(isWhitePiece(piece), true);
  }
  if (castleQueenSide) {
    moveRookForCastling(isWhitePiece(piece), false);
  }

  if (piece == 'K') {
    whiteKingMoved = true;
  }
  if (piece == 'k') {
    blackKingMoved = true;
  }
  if (piece == 'R' && fromFile == 0 && fromRank == 0) {
    whiteRookAMoved = true;
  }
  if (piece == 'R' && fromFile == 7 && fromRank == 0) {
    whiteRookHMoved = true;
  }
  if (piece == 'r' && fromFile == 0 && fromRank == 7) {
    blackRookAMoved = true;
  }
  if (piece == 'r' && fromFile == 7 && fromRank == 7) {
    blackRookHMoved = true;
  }

  if (piece == 'P' && toRank == 7) {
    board[toFile][toRank] = 'Q';
    registerPiece(movedUid, 'Q');
    promoted = true;
  }
  if (piece == 'p' && toRank == 0) {
    board[toFile][toRank] = 'q';
    registerPiece(movedUid, 'q');
    promoted = true;
  }

  enPassantFile = -1;
  enPassantRank = -1;
  if (piece == 'P' && fromRank == 1 && toRank == 3 && fromFile == toFile) {
    enPassantFile = fromFile;
    enPassantRank = 2;
  }
  if (piece == 'p' && fromRank == 6 && toRank == 4 && fromFile == toFile) {
    enPassantFile = fromFile;
    enPassantRank = 5;
  }

  if (piece == 'P' || piece == 'p' || isCapture) {
    halfmoveClock = 0;
  } else {
    halfmoveClock++;
  }

  if (whiteTurn) {
    whiteTurn = false;
  } else {
    whiteTurn = true;
    fullmoveNumber++;
  }

  sanitizeSquareUIDByBoard();
  rebuildOccupiedListFromBoard();

  String moveText = squareNameFromIdx(fromIdx) + squareNameFromIdx(toIdx);
  if (castleKingSide) {
    moveText = "O-O";
  }
  if (castleQueenSide) {
    moveText = "O-O-O";
  }
  if (promoted) {
    moveText += "q";
  }
  if (enPassantCapture) {
    moveText += " ep";
  }

  Serial.print(F("[MOVE] "));
  Serial.println(moveText);
  bleFenLog(String("[MOVE] ") + moveText + String(" | ") + squareNameFromIdx(fromIdx) + String("->") + squareNameFromIdx(toIdx));
  printBoard(board);
  printFEN();
  return true;
}

static void showCandidates() {
  Serial.print(F("[CANDIDATES] "));
  Serial.print(candidateCount);
  Serial.print(F(" o: "));
  for (int i = 0; i < candidateCount; i++) {
    Serial.print(squareNameFromIdx(candidateList[i]));
    if (i + 1 < candidateCount) {
      Serial.print(' ');
    }
  }
  Serial.println();
}

static bool isIdxInCandidates(int idx) {
  for (int i = 0; i < candidateCount; i++) {
    if (candidateList[i] == idx) {
      return true;
    }
  }
  return false;
}

static bool findLiftedUidInList(const int *list, int count, int skipIdx, int &foundIdx) {
  for (int i = 0; i < count; i++) {
    int idx = list[i];
    if (idx == skipIdx) {
      continue;
    }
    String uid = timedScanSquare(idx);
    if (uid == liftedUID) {
      foundIdx = idx;
      return true;
    }
  }
  foundIdx = -1;
  return false;
}

static void handleScanIdle() {
  for (int i = 0; i < occupiedCount; i++) {
    int idx = occupiedList[i];
    String uid = timedScanSquare(idx);

    if (uid == squareUID[idx]) {
      continue;
    }

    if (uid.length() == 0 && squareUID[idx].length() > 0) {
      setPendingLift(idx, squareUID[idx]);
      return;
    }

    if (uid.length() > 0) {
      squareUID[idx] = uid;
    }
  }

  delay(IDLE_SLEEP_MS);
}

static void handleLiftPending() {
  if (pendingLiftIdx < 0 || pendingLiftIdx >= NUM_ANTENNAS || pendingLiftUID.length() == 0) {
    clearPendingLift();
    scanState = SCAN_IDLE;
    return;
  }

  String uid = timedScanSquare(pendingLiftIdx);
  if (uid == pendingLiftUID) {
    clearPendingLift();
    scanState = SCAN_IDLE;
    return;
  }

  if (uid.length() > 0) {
    squareUID[pendingLiftIdx] = uid;
    clearPendingLift();
    scanState = SCAN_IDLE;
    return;
  }

  if (millis() - pendingLiftFirstMs < DEBOUNCE_LIFT_MS) {
    return;
  }

  liftedFromIdx = pendingLiftIdx;
  liftedUID = pendingLiftUID;
  liftedPiece = board[liftedFromIdx / 8][liftedFromIdx % 8];
  if (liftedPiece == '.') {
    liftedPiece = lookupUID(liftedUID);
  }
  liftDetectedMs = millis();
  clearPendingLift();

  Serial.print(F("[LIFT] "));
  Serial.print(squareNameFromIdx(liftedFromIdx));
  Serial.print(F(" uid="));
  Serial.println(liftedUID);
  bleFenLog(String("[LIFT] ") + squareNameFromIdx(liftedFromIdx) + String(" piece lifted"));

  scanState = SCAN_PIECE_LIFTED;
}

static void handlePieceLifted() {
  if (millis() - liftDetectedMs < DEBOUNCE_LIFT_MS) {
    return;
  }

  String recheck = timedScanSquare(liftedFromIdx);
  if (recheck == liftedUID) {
    Serial.println(F("[LIFT] false-lift"));
    bleFenLog(String("[INFO] LIFT_GLITCH at ") + squareNameFromIdx(liftedFromIdx) + String(" | scan glitch ignored"));
    abortTrackingRestoreSource();
    return;
  }

  bool liftedIsWhite = isWhitePiece(liftedPiece);
  if ((whiteTurn && !liftedIsWhite) || (!whiteTurn && liftedIsWhite)) {
    Serial.println(F("[TRACK] wrong turn -> fallback"));
    bleFenLog(String("[ERR] WRONG_TURN expected=") + sideName(whiteTurn) +
              String(" got=") + sideName(liftedIsWhite) +
              String(" at ") + squareNameFromIdx(liftedFromIdx) +
              String(" | place piece back"));
    fallbackStartMs = millis();
    lastFallbackTryMs = 0;
    candidateCount = 0;
    scanState = SCAN_FALLBACK;
    return;
  }

  squareUID[liftedFromIdx] = "";

  int fromFile = liftedFromIdx / 8;
  int fromRank = liftedFromIdx % 8;
  bool canWk = (!whiteKingMoved && !whiteRookHMoved && board[7][0] == 'R');
  bool canWq = (!whiteKingMoved && !whiteRookAMoved && board[0][0] == 'R');
  bool canBk = (!blackKingMoved && !blackRookHMoved && board[7][7] == 'r');
  bool canBq = (!blackKingMoved && !blackRookAMoved && board[0][7] == 'r');

  generateCandidateSquares(board,
                           fromFile,
                           fromRank,
                           liftedPiece,
                           enPassantFile,
                           enPassantRank,
                           canWk,
                           canWq,
                           canBk,
                           canBq,
                           candidateList,
                           candidateCount);

  if (candidateCount == 0) {
    Serial.println(F("[TRACK] no candidates -> fallback"));
    bleFenLog(String("[ERR] NO_LEGAL_DEST from ") + squareNameFromIdx(liftedFromIdx) + String(" | reset piece then retry"));
    fallbackStartMs = millis();
    lastFallbackTryMs = 0;
    scanState = SCAN_FALLBACK;
    return;
  }

  showCandidates();
  trackingStartMs = millis();
  scanState = SCAN_TRACKING_DESTINATION;
}

static void handleTrackingDestination() {
  String src = timedScanSquare(liftedFromIdx);
  if (src == liftedUID) {
    Serial.println(F("[TRACK] returned source"));
    bleFenLog(String("[INFO] RETURNED_SOURCE ") + squareNameFromIdx(liftedFromIdx));
    abortTrackingRestoreSource();
    return;
  }

  for (int i = 0; i < candidateCount; i++) {
    int idx = candidateList[i];
    String uid = timedScanSquare(idx);
    if (uid == liftedUID) {
      liftedToIdx = idx;
      verifyCount = 0;
      Serial.print(F("[TRACK] found at "));
      Serial.println(squareNameFromIdx(liftedToIdx));
      bleFenLog(String("[TRACK] TARGET ") + squareNameFromIdx(liftedToIdx));
      scanState = SCAN_VERIFY;
      return;
    }
  }

  if (millis() - trackingStartMs > TRACKING_TIMEOUT_MS) {
    Serial.println(F("[TRACK] timeout -> fallback"));
    bleFenLog(String("[ERR] TRACK_TIMEOUT from ") + squareNameFromIdx(liftedFromIdx) + String(" | place piece correctly"));
    fallbackStartMs = millis();
    lastFallbackTryMs = 0;
    scanState = SCAN_FALLBACK;
  }
}

static void handleVerify() {
  String src = timedScanSquare(liftedFromIdx);
  String dst = timedScanSquare(liftedToIdx);

  if (src.length() == 0 && dst == liftedUID) {
    verifyCount++;
    Serial.print(F("[VERIFY] "));
    Serial.print(verifyCount);
    Serial.print('/');
    Serial.println(VERIFY_ROUNDS);
  } else {
    verifyCount = 0;
    scanState = SCAN_TRACKING_DESTINATION;
    return;
  }

  if (verifyCount >= VERIFY_ROUNDS) {
    if (applyMove(liftedFromIdx, liftedToIdx, liftedUID)) {
      totalMovesCount++;
      resetTrackingState();
      bleFenLog(String("[OK] MOVE_CONFIRMED"));
    } else {
      verifyCount = 0;
      fallbackStartMs = millis();
      lastFallbackTryMs = 0;
      scanState = SCAN_FALLBACK;
    }
  }
}

static void handleFallback() {
  unsigned long now = millis();
  if (now - fallbackStartMs > FALLBACK_TIMEOUT_MS) {
    Serial.println(F("[FALLBACK] timeout, abandon"));
    bleFenLog(String("[ERR] FALLBACK_TIMEOUT | place piece back and retry"));
    abortTrackingRestoreSource();
    return;
  }

  if (lastFallbackTryMs != 0 && (now - lastFallbackTryMs < FALLBACK_RETRY_DELAY_MS)) {
    return;
  }
  lastFallbackTryMs = now;

  String src = timedScanSquare(liftedFromIdx);
  if (src == liftedUID) {
    Serial.println(F("[FALLBACK] returned source"));
    bleFenLog(String("[INFO] FALLBACK_RETURN_SOURCE"));
    abortTrackingRestoreSource();
    return;
  }

  int foundIdx = -1;
  if (findLiftedUidInList(candidateList, candidateCount, liftedFromIdx, foundIdx)) {
    liftedToIdx = foundIdx;
    verifyCount = 0;
    Serial.print(F("[FALLBACK] found in candidates: "));
    Serial.println(squareNameFromIdx(liftedToIdx));
    bleFenLog(String("[FALLBACK] FOUND_CANDIDATE ") + squareNameFromIdx(liftedToIdx));
    scanState = SCAN_VERIFY;
    return;
  }

  if (findLiftedUidInList(occupiedList, occupiedCount, liftedFromIdx, foundIdx)) {
    if (!isIdxInCandidates(foundIdx)) {
      Serial.print(F("[FALLBACK] found outside candidates: "));
      Serial.println(squareNameFromIdx(foundIdx));
      bleFenLog(String("[WARN] FOUND_OUTSIDE_CANDIDATE ") + squareNameFromIdx(foundIdx) + String(" | verify board placement"));
    }
    liftedToIdx = foundIdx;
    verifyCount = 0;
    scanState = SCAN_VERIFY;
  }
}

static void runScanStateMachine() {
  switch (scanState) {
    case SCAN_IDLE:
      handleScanIdle();
      break;
    case SCAN_LIFT_PENDING:
      handleLiftPending();
      break;
    case SCAN_PIECE_LIFTED:
      handlePieceLifted();
      break;
    case SCAN_TRACKING_DESTINATION:
      handleTrackingDestination();
      break;
    case SCAN_VERIFY:
      handleVerify();
      break;
    case SCAN_FALLBACK:
      handleFallback();
      break;
  }
}

static void printHelp() {
  Serial.println(F("[CMD] START | STOP | STATUS | F | S | V | C | T | B | L | HELP"));
}

static bool handleBleCommand(const String &raw, String &response) {
  String cmd = raw;
  cmd.trim();
  cmd.toUpperCase();

  if (cmd.length() == 0) {
    response = "EMPTY";
    return false;
  }

  if (cmd == "START") {
    if (startAndLearnInitial32()) {
      response = "STARTED";
      return true;
    }
    if (startFailDetail.length() > 0) {
      response = String("START_FAILED:") + startFailReason + String(":") + startFailDetail;
    } else {
      response = String("START_FAILED:") + startFailReason;
    }
    bleFenLog(String("[ERR] START_FAILED reason=") + startFailReason + String(" detail=") + (startFailDetail.length() ? startFailDetail : String("none")) + String(" | check initial board and retry"));
    return false;
  }

  if (cmd == "STOP") {
    gameStarted = false;
    abortTrackingRestoreSource();

    unsigned long gameDurationMs = 0;
    if (gameStartTimeMs > 0) {
      gameDurationMs = millis() - gameStartTimeMs;
    }
    int mins = (gameDurationMs / 60000);
    int secs = (gameDurationMs / 1000) % 60;

    String result = "STOPPED";
    result += "|moves=";
    result += String(totalMovesCount);
    result += "|time=";
    result += String(mins);
    result += "m";
    result += String(secs);
    result += "s";
    result += "|fen=";
    result += buildCurrentFen();

    response = result;
    bleFenLog(String("[INFO] STOPPED moves=") + String(totalMovesCount) + String(" time=") + String(mins) + String("m") + String(secs) + String("s"));
    return true;
  }

  if (cmd == "STATUS") {
    response = gameStarted ? "RUNNING" : "IDLE";
    return true;
  }

  if (cmd == "F" || cmd == "FEN") {
    response = gameStarted ? buildCurrentFen() : String("NO_GAME");
    if (!gameStarted) {
      bleFenLog(String("[WARN] NO_GAME | send START first"));
    }
    return gameStarted;
  }

  response = "UNKNOWN_CMD";
  bleFenLog(String("[ERR] UNKNOWN_CMD ") + cmd);
  return false;
}

static void printStatus() {
  Serial.print(F("[STATUS] gameStarted="));
  Serial.print(gameStarted ? F("YES") : F("NO"));
  Serial.print(F(" occupied="));
  Serial.print(occupiedCount);
  Serial.print(F(" state="));
  Serial.println((int)scanState);
}

static void handleCommand(const String &raw) {
  String cmd = raw;
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) {
    return;
  }

  if (cmd == "HELP") {
    printHelp();
    return;
  }

  if (cmd == "START") {
    if (startAndLearnInitial32()) {
      Serial.println(F("[READY] Game started. Scan toi uu theo so quan hien co."));
    } else {
      Serial.println(F("[START] Khong the bat dau. Kiem tra vi tri 32 quan roi START lai."));
    }
    return;
  }

  if (cmd == "STOP") {
    gameStarted = false;
    abortTrackingRestoreSource();
    Serial.println(F("[STOP] Da dung game. Gui START de bat dau lai."));
    return;
  }

  if (cmd == "STATUS") {
    printStatus();
    return;
  }

  if (cmd == "F" || cmd == "FEN") {
    if (!gameStarted) {
      Serial.println(F("[FEN] Chua START."));
    } else {
      printFEN();
    }
    return;
  }

  if (cmd == "S" || cmd == "SNAP") {
    fullScanToBuffer(mfrc522,
                     scanBuf,
                     lastScannedCount,
                     lastFullScanUs,
                     lastAvgCellUs,
                     lastMinCellUs,
                     lastMaxCellUs);
    printScanSnapshot(scanBuf, F("[SCAN] Snapshot 64 o (thu cong)"));
    return;
  }

  if (cmd == "V") {
    scanVerbose = !scanVerbose;
    Serial.print(F("[SCAN] Verbose = "));
    Serial.println(scanVerbose ? F("ON") : F("OFF"));
    return;
  }

  if (cmd == "C") {
    scanContinuous = !scanContinuous;
    Serial.print(F("[SCAN] Continuous = "));
    Serial.println(scanContinuous ? F("ON") : F("OFF"));
    return;
  }

  if (cmd == "L") {
    scanUsePieceLetters = !scanUsePieceLetters;
    Serial.print(F("[SCAN] Letter view = "));
    Serial.println(scanUsePieceLetters ? F("ON") : F("OFF"));
    return;
  }

  if (cmd == "T") {
    printScanTiming(lastScannedCount, lastFullScanUs, lastAvgCellUs, lastMinCellUs, lastMaxCellUs);
    return;
  }

  if (cmd == "B") {
    Serial.print(F("[BLE] connected="));
    Serial.println(bleFenIsConnected() ? F("YES") : F("NO"));
    return;
  }

  Serial.print(F("[CMD] Unknown: "));
  Serial.println(cmd);
  printHelp();
}

static void processSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (cmdBuffer.length() > 0) {
        handleCommand(cmdBuffer);
        cmdBuffer = "";
        cmdLastByteMs = 0;
      }
      continue;
    }

    if (cmdBuffer.length() < 64) {
      cmdBuffer += c;
      cmdLastByteMs = millis();
    }
  }

  if (cmdBuffer.length() > 0 && cmdLastByteMs > 0) {
    if (millis() - cmdLastByteMs >= CMD_COMMIT_IDLE_MS) {
      handleCommand(cmdBuffer);
      cmdBuffer = "";
      cmdLastByteMs = 0;
    }
  }
}

void smartChessBegin() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 3000) {
  }

  initRfidScanner(mfrc522);
  bleFenBegin();
  bleFenSetCommandHandler(handleBleCommand);

  byte version = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println(F("Khong tim thay RC522!"));
    while (1) {
    }
  }

  prepareNewGameModel();
  gameStarted = false;

  Serial.println(F("[BOOT] He thong san sang."));
  Serial.println(F("[BOOT] Dat quan dung vi tri ban dau, gui START de bat dau."));
  printHelp();
}

void smartChessTick() {
  bleFenPoll();
  processSerialCommands();

  if (!gameStarted) {
    delay(1);
    return;
  }

  beginCycleScanMetrics();
  runScanStateMachine();
  endCycleScanMetrics();

  unsigned long now = millis();
  if (scanContinuous && (now - lastScanPrintMs >= SCAN_PRINT_INTERVAL_MS)) {
    printScanCompact(squareUID, scanVerbose, board, scanUsePieceLetters);
    lastScanPrintMs = now;
  }

  if (now - lastTimingPrintMs >= TIMING_PRINT_INTERVAL_MS) {
    printScanTiming(lastScannedCount, lastFullScanUs, lastAvgCellUs, lastMinCellUs, lastMaxCellUs);
    lastTimingPrintMs = now;
  }

  if (bleFenIsConnected() && (now - lastBleKeepAliveMs >= BLE_KEEPALIVE_MS)) {
    bleFenPublish(buildCurrentFen());
    lastBleKeepAliveMs = now;
  }

  if (scanVerbose && scanState != SCAN_IDLE) {
    Serial.print(F("[STATE] "));
    Serial.println((int)scanState);
  }

  delay(1);
}
