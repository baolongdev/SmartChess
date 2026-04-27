#include "SmartChessApp.h"

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <MFRC522.h>
#include <Preferences.h>
#include <WiFi.h>

#include "BoardState.h"
#include "BleFen.h"
#include "Fen.h"
#include "LichessPublish.h"
#include "MoveGen.h"
#include "RfidScanner.h"
#include "ScanDebug.h"
#include "SmartChessConfig.h"
#include "SmartChessTypes.h"
#include "TextUtils.h"

#include <ctype.h>

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

static Preferences settingsStore;
static bool settingsStoreReady = false;
static uint32_t settingsVersion = 1;
static uint32_t wifiSettingsVersion = 1;
constexpr const char *SETTINGS_NS = "smartchess";
constexpr const char *KEY_CFG_VER = "cfg_ver";
constexpr const char *KEY_SCAN_V = "scan_v";
constexpr const char *KEY_SCAN_C = "scan_c";
constexpr const char *KEY_SCAN_L = "scan_l";
constexpr const char *KEY_WIFI_VER = "wifi_ver";
constexpr const char *KEY_WIFI_SSID = "wifi_ssid";
constexpr const char *KEY_WIFI_PASS = "wifi_pass";
constexpr const char *KEY_WIFI_AUTO = "wifi_auto";

static String wifiSavedSsid = "";
static String wifiSavedPass = "";
static bool wifiAutoConnect = true;
static bool wifiConnectInProgress = false;
static unsigned long wifiConnectStartMs = 0;
static bool wifiLastConnected = false;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr int WIFI_SCAN_MAX = 24;
static String wifiScanResults[WIFI_SCAN_MAX];
static int wifiScanCount = 0;
static String wifiScanLastError = "";
static bool wifiConnectFailed = false;
static String wifiLastError = "";

static char lichessMovesBuf[512];
static int lichessMovesLen = 0;
static int lichessPlyCount = 0;
static String lastLichessUrl = "";
static String lastLichessError = "";
static bool lichessRepublishPending = false;
static bool lichessAutoPublishPerMove = false;

static int lichessUploadState = LICHESS_IDLE;
static char lichessPayloadBuf[192];

static bool startWifiConnect();
static bool handleWifiSetCommand(const String &cmd, String &response);
static const char* buildLichessPayload(const char* prefix);
static bool queueLichessPublish(String &response);
static void printCfgStatus();
static void printWifiStatus();

static LichessContext makeLichessContext() {
  LichessContext ctx = {};
  ctx.movesBuf = lichessMovesBuf;
  ctx.movesBufSize = (int)sizeof(lichessMovesBuf);
  ctx.movesLen = &lichessMovesLen;
  ctx.plyCount = &lichessPlyCount;
  ctx.lastUrl = &lastLichessUrl;
  ctx.lastError = &lastLichessError;
  ctx.republishPending = &lichessRepublishPending;
  ctx.autoPublishPerMove = &lichessAutoPublishPerMove;
  ctx.uploadState = &lichessUploadState;
  ctx.whiteKingMoved = whiteKingMoved;
  ctx.blackKingMoved = blackKingMoved;
  ctx.whiteRookAMoved = whiteRookAMoved;
  ctx.whiteRookHMoved = whiteRookHMoved;
  ctx.blackRookAMoved = blackRookAMoved;
  ctx.blackRookHMoved = blackRookHMoved;
  ctx.board = board;
  ctx.enPassantFile = enPassantFile;
  ctx.enPassantRank = enPassantRank;
  return ctx;
}

static String wifiStatusText() {
  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    return String("CONNECTED");
  }
  if (wifiConnectFailed) {
    return String("FAILED");
  }
  if (wifiConnectInProgress) {
    return String("CONNECTING");
  }
  return String("DISCONNECTED");
}

static String wifiIpText() {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return String("0.0.0.0");
}

static int performWifiScan() {
  bool resumeConnectAfterScan = wifiConnectInProgress;
  if (resumeConnectAfterScan) {
    wifiConnectInProgress = false;
  }

  WiFi.mode(WIFI_STA);
  delay(40);
  WiFi.scanDelete();

  int n = -1;
  for (int attempt = 0; attempt < 3; attempt++) {
    n = WiFi.scanNetworks(false, true);
    if (n >= 0) {
      break;
    }
    delay(120 + attempt * 80);
  }

  if (n < 0) {
    wifiScanLastError = "SCAN_FAIL";
    n = 0;
  } else if (n == 0) {
    wifiScanLastError = "NO_AP";
  } else {
    wifiScanLastError = "";
  }

  if (n > WIFI_SCAN_MAX) {
    n = WIFI_SCAN_MAX;
  }

  wifiScanCount = n;
  for (int i = 0; i < wifiScanCount; i++) {
    String ssid = WiFi.SSID(i);
    ssid.replace("|", " ");
    wifiScanResults[i] = ssid;
  }
  for (int i = wifiScanCount; i < WIFI_SCAN_MAX; i++) {
    wifiScanResults[i] = "";
  }

  WiFi.scanDelete();

  if (resumeConnectAfterScan && WiFi.status() != WL_CONNECTED) {
    wifiConnectInProgress = true;
  }

  return wifiScanCount;
}

static String buildWifiScanPayload(const String &prefix) {
  String payload = prefix;
  payload += "|count=";
  payload += String(wifiScanCount);
  payload += "|err=";
  payload += wifiScanLastError;
  return payload;
}

static String buildWifiScanItemPayload(int idx, const String &prefix) {
  String payload = prefix;
  payload += "|idx=";
  payload += String(idx);
  payload += "|ssid=";
  if (idx >= 0 && idx < wifiScanCount) {
    payload += wifiScanResults[idx];
  }
  return payload;
}

static char wifiPayloadBuf[128];

static const char* buildWifiPayload(const char* prefix) {
  snprintf(wifiPayloadBuf, sizeof(wifiPayloadBuf),
    "%s|ver=%lu|ssid=%s|auto=%d|status=%s|ip=%s|err=%s",
    prefix, wifiSettingsVersion,
    wifiSavedSsid.c_str(),
    wifiAutoConnect ? 1 : 0,
    wifiStatusText().c_str(),
    wifiIpText().c_str(),
    wifiLastError.c_str());
  return wifiPayloadBuf;
}

static void persistWifiSettingsToStore() {
  if (!settingsStoreReady) {
    return;
  }

  settingsStore.putULong(KEY_WIFI_VER, wifiSettingsVersion);
  settingsStore.putString(KEY_WIFI_SSID, wifiSavedSsid);
  settingsStore.putString(KEY_WIFI_PASS, wifiSavedPass);
  settingsStore.putBool(KEY_WIFI_AUTO, wifiAutoConnect);
}

static void loadWifiSettingsFromStore() {
  if (!settingsStoreReady) {
    return;
  }

  wifiSettingsVersion = settingsStore.getULong(KEY_WIFI_VER, 1UL);
  if (wifiSettingsVersion == 0) {
    wifiSettingsVersion = 1;
  }

  wifiSavedSsid = settingsStore.getString(KEY_WIFI_SSID, "");
  wifiSavedPass = settingsStore.getString(KEY_WIFI_PASS, "");
  wifiAutoConnect = settingsStore.getBool(KEY_WIFI_AUTO, true);
}

static void bumpWifiSettingsVersion() {
  if (wifiSettingsVersion >= 0xFFFFFFFFUL) {
    wifiSettingsVersion = 1;
  } else {
    wifiSettingsVersion++;
  }
}

static bool startWifiConnect() {
  if (wifiSavedSsid.length() == 0) {
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(60);
  WiFi.begin(wifiSavedSsid.c_str(), wifiSavedPass.c_str());
  wifiConnectInProgress = true;
  wifiConnectFailed = false;
  wifiLastError = "";
  wifiConnectStartMs = millis();
  bleFenLog(String("[WIFI] CONNECTING ssid=") + wifiSavedSsid);
  return true;
}

static void disconnectWifiNow() {
  WiFi.disconnect(true);
  wifiConnectInProgress = false;
  wifiConnectFailed = false;
  wifiLastError = "";
  wifiConnectStartMs = 0;
  wifiLastConnected = false;
}

static char settingsPayloadBuf[64];

static const char* buildSettingsPayload(const char* prefix) {
  snprintf(settingsPayloadBuf, sizeof(settingsPayloadBuf),
    "%s|ver=%lu|verbose=%d|continuous=%d|letters=%d",
    prefix, settingsVersion,
    scanVerbose ? 1 : 0,
    scanContinuous ? 1 : 0,
    scanUsePieceLetters ? 1 : 0);
  return settingsPayloadBuf;
}

static void bumpSettingsVersion() {
  if (settingsVersion >= 0xFFFFFFFFUL) {
    settingsVersion = 1;
  } else {
    settingsVersion++;
  }
}

static void persistSettingsToStore() {
  if (!settingsStoreReady) {
    return;
  }

  settingsStore.putULong(KEY_CFG_VER, settingsVersion);
  settingsStore.putBool(KEY_SCAN_V, scanVerbose);
  settingsStore.putBool(KEY_SCAN_C, scanContinuous);
  settingsStore.putBool(KEY_SCAN_L, scanUsePieceLetters);
}

static void loadSettingsFromStore() {
  if (!settingsStoreReady) {
    return;
  }

  settingsVersion = settingsStore.getULong(KEY_CFG_VER, 1UL);
  if (settingsVersion == 0) {
    settingsVersion = 1;
  }

  scanVerbose = settingsStore.getBool(KEY_SCAN_V, false);
  scanContinuous = settingsStore.getBool(KEY_SCAN_C, true);
  scanUsePieceLetters = settingsStore.getBool(KEY_SCAN_L, true);
}

static void saveSettingsAfterLocalChange() {
  bumpSettingsVersion();
  persistSettingsToStore();
}

static bool handleCfgSetCommand(const String &cmd, String &response) {
  bool hasAnyField = false;
  bool boolValue = false;

  String vVerbose = settingsFieldValue(cmd, "VERBOSE");
  if (vVerbose.length() > 0) {
    if (!parseBoolToken(vVerbose, boolValue)) {
      response = "CFG_ERR:BAD_VERBOSE";
      return false;
    }
    scanVerbose = boolValue;
    hasAnyField = true;
  }

  String vContinuous = settingsFieldValue(cmd, "CONTINUOUS");
  if (vContinuous.length() > 0) {
    if (!parseBoolToken(vContinuous, boolValue)) {
      response = "CFG_ERR:BAD_CONTINUOUS";
      return false;
    }
    scanContinuous = boolValue;
    hasAnyField = true;
  }

  String vLetters = settingsFieldValue(cmd, "LETTERS");
  if (vLetters.length() > 0) {
    if (!parseBoolToken(vLetters, boolValue)) {
      response = "CFG_ERR:BAD_LETTERS";
      return false;
    }
    scanUsePieceLetters = boolValue;
    hasAnyField = true;
  }

  if (!hasAnyField) {
    response = "CFG_ERR:NO_FIELDS";
    return false;
  }

  uint32_t incomingVer = 0;
  String vVersion = settingsFieldValue(cmd, "VER");
  if (vVersion.length() > 0) {
    incomingVer = (uint32_t)strtoul(vVersion.c_str(), nullptr, 10);
  }

  if (incomingVer > settingsVersion) {
    settingsVersion = incomingVer;
  } else {
    bumpSettingsVersion();
  }

  persistSettingsToStore();
  response = buildSettingsPayload("CFG_SAVED");
  bleFenLog(String("[CFG] SAVED ") + response);
  return true;
}

static void applyLichessStreamCommand(bool enabled, String &response) {
  lichessAutoPublishPerMove = enabled;
  response = buildLichessPayload(enabled ? "LICHESS_STREAM_ON" : "LICHESS_STREAM_OFF");
}

static bool handleCommonCommand(const String &cmd,
                                const String &cmdUpper,
                                String &response,
                                bool &handled) {

  if (cmdUpper == "CFG" || cmdUpper == "CFG?" || cmdUpper == "CFG_GET") {
    handled = true;
    response = buildSettingsPayload("CFG");
    return true;
  }

  if (cmdUpper.startsWith("CFG_SET")) {
    handled = true;
    return handleCfgSetCommand(cmd, response);
  }

  if (cmdUpper == "WIFI" || cmdUpper == "WIFI?" || cmdUpper == "WIFI_GET") {
    handled = true;
    response = buildWifiPayload("WIFI");
    return true;
  }

  if (cmdUpper == "WIFI_SCAN") {
    handled = true;
    performWifiScan();
    response = buildWifiScanPayload("WIFI_SCAN");
    return true;
  }

  if (cmdUpper.startsWith("WIFI_SCAN_ITEM")) {
    handled = true;
    String idxToken = settingsFieldValue(cmd, "IDX");
    if (idxToken.length() == 0) {
      response = "WIFI_ERR:BAD_IDX";
      return false;
    }

    int idx = idxToken.toInt();
    if (idx < 0 || idx >= wifiScanCount) {
      response = "WIFI_ERR:IDX_RANGE";
      return false;
    }
    response = buildWifiScanItemPayload(idx, "WIFI_SCAN_ITEM");
    return true;
  }

  if (cmdUpper.startsWith("WIFI_SET")) {
    handled = true;
    return handleWifiSetCommand(cmd, response);
  }

  if (cmdUpper == "WIFI_CONNECT") {
    handled = true;
    if (startWifiConnect()) {
      response = buildWifiPayload("WIFI_CONNECTING");
      return true;
    }
    response = "WIFI_ERR:EMPTY_SSID";
    return false;
  }

  if (cmdUpper == "WIFI_DISCONNECT") {
    handled = true;
    disconnectWifiNow();
    response = buildWifiPayload("WIFI_DISCONNECTED");
    return true;
  }

  if (cmdUpper == "LICHESS" || cmdUpper == "LICHESS_SEND" || cmdUpper == "LICHESS_PUBLISH") {
    handled = true;
    return queueLichessPublish(response);
  }

  if (cmdUpper == "LICHESS_STREAM_ON") {
    handled = true;
    applyLichessStreamCommand(true, response);
    return true;
  }

  if (cmdUpper == "LICHESS_STREAM_OFF") {
    handled = true;
    applyLichessStreamCommand(false, response);
    return true;
  }

  if (cmdUpper == "LICHESS_STATUS" || cmdUpper == "LICHESS?") {
    handled = true;
    response = buildLichessPayload("LICHESS");
    return true;
  }

  handled = false;
  return false;
}

static bool handleWifiSetCommand(const String &cmd, String &response) {
  bool hasAnyField = false;
  bool boolValue = false;

  String vSsid = settingsFieldValue(cmd, "SSID");
  String vPass = settingsFieldValue(cmd, "PASS");
  String vAuto = settingsFieldValue(cmd, "AUTO");
  String vConnect = settingsFieldValue(cmd, "CONNECT");

  String cmdUpper = cmd;
  cmdUpper.toUpperCase();

  if (vSsid.length() > 0 || cmdUpper.indexOf("SSID=") >= 0) {
    wifiSavedSsid = vSsid;
    hasAnyField = true;
  }

  if (vPass.length() > 0 || cmdUpper.indexOf("PASS=") >= 0) {
    wifiSavedPass = vPass;
    hasAnyField = true;
  }

  if (vAuto.length() > 0) {
    if (!parseBoolToken(vAuto, boolValue)) {
      response = "WIFI_ERR:BAD_AUTO";
      return false;
    }
    wifiAutoConnect = boolValue;
    hasAnyField = true;
  }

  if (!hasAnyField && vConnect.length() == 0) {
    response = "WIFI_ERR:NO_FIELDS";
    return false;
  }

  uint32_t incomingVer = 0;
  String vVersion = settingsFieldValue(cmd, "VER");
  if (vVersion.length() > 0) {
    incomingVer = (uint32_t)strtoul(vVersion.c_str(), nullptr, 10);
  }

  if (incomingVer > wifiSettingsVersion) {
    wifiSettingsVersion = incomingVer;
  } else if (hasAnyField) {
    bumpWifiSettingsVersion();
  }

  if (hasAnyField) {
    persistWifiSettingsToStore();
  }

  bool forceConnect = false;
  if (vConnect.length() > 0) {
    if (!parseBoolToken(vConnect, boolValue)) {
      response = "WIFI_ERR:BAD_CONNECT";
      return false;
    }
    forceConnect = boolValue;
  }

  if (forceConnect || wifiAutoConnect) {
    if (wifiSavedSsid.length() == 0) {
      response = "WIFI_ERR:EMPTY_SSID";
      return false;
    }
    startWifiConnect();
  }

  response = buildWifiPayload("WIFI_SAVED");
  bleFenLog(String("[WIFI] SAVED ") + response);
  return true;
}

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

static char fenBuffer[128];

static const char* buildCurrentFen() {
  currentFEN(board,
             fenBuffer,
             sizeof(fenBuffer),
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
  return fenBuffer;
}

static void resetLichessTracking() {
  LichessContext ctx = makeLichessContext();
  lichessResetTracking(ctx);
}

static const char* buildLichessPayload(const char* prefix) {
  LichessContext ctx = makeLichessContext();
  return lichessBuildPayload(ctx, prefix, lichessPayloadBuf, sizeof(lichessPayloadBuf));
}

static bool queueLichessPublish(String &response) {
  LichessContext ctx = makeLichessContext();
  return lichessQueuePublish(ctx, response, bleFenLog);
}

static void requestLichessAutoPublish() {
  LichessContext ctx = makeLichessContext();
  lichessRequestAutoPublish(ctx, bleFenLog);
}

static void processLichessUploadTick() {
  LichessContext ctx = makeLichessContext();
  lichessProcessUploadTick(ctx, bleFenLog);
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
  resetLichessTracking();
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

  bool willPromoteToQueen = ((piece == 'P' && toRank == 7) || (piece == 'p' && toRank == 0));
  LichessContext lichessCtx = makeLichessContext();
  String lichessSanMove = lichessBuildSanMove(lichessCtx,
                                              fromIdx,
                                              toIdx,
                                              piece,
                                              isCapture,
                                              castleKingSide,
                                              castleQueenSide,
                                              enPassantCapture,
                                              willPromoteToQueen);

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

  lichessCtx = makeLichessContext();
  if (!lichessAppendMove(lichessCtx, lichessSanMove)) {
    bleFenLog(String("[WARN] LICHESS_APPEND_FAILED move=") + lichessSanMove + String(" raw=") + moveText);
  }

  requestLichessAutoPublish();

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
  Serial.println(F("[CMD] START | STOP | STATUS | F | S | V | C | T | B | L | CFG | CFG_SET|VERBOSE=0|CONTINUOUS=1|LETTERS=1 | WIFI | WIFI_SET|SSID=...|PASS=...|AUTO=1|CONNECT=1 | WIFI_SCAN | WIFI_SCAN_ITEM|IDX=n | LICHESS_PUBLISH | LICHESS_STATUS | LICHESS_STREAM_ON | LICHESS_STREAM_OFF | HELP"));
}

static bool handleBleCommand(const String &raw, String &response) {
  String cmd = raw;
  cmd.trim();

  String cmdUpper = cmd;
  cmdUpper.toUpperCase();

  if (cmdUpper.length() == 0) {
    response = "EMPTY";
    return false;
  }

  if (cmdUpper == "START") {
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

  if (cmdUpper == "STOP") {
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

  if (cmdUpper == "STATUS") {
    response = gameStarted ? "RUNNING" : "IDLE";
    return true;
  }

  bool handled = false;
  if (handleCommonCommand(cmd, cmdUpper, response, handled)) {
    return true;
  }
  if (handled) {
    return false;
  }

  if (cmdUpper == "F" || cmdUpper == "FEN") {
    if (gameStarted) {
      response = String(buildCurrentFen());
    } else {
      response = "NO_GAME";
    }
    if (!gameStarted) {
      bleFenLog(String("[WARN] NO_GAME | send START first"));
    }
    return gameStarted;
  }

  response = "UNKNOWN_CMD";
  bleFenLog(String("[ERR] UNKNOWN_CMD ") + cmdUpper);
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

static void printCfgStatus() {
  Serial.print(F("[CFG] "));
  Serial.println(buildSettingsPayload("CFG"));
}

static void printWifiStatus() {
  Serial.print(F("[WIFI] "));
  Serial.println(buildWifiPayload("WIFI"));
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
    saveSettingsAfterLocalChange();
    Serial.print(F("[SCAN] Verbose = "));
    Serial.println(scanVerbose ? F("ON") : F("OFF"));
    printCfgStatus();
    return;
  }

  if (cmd == "C") {
    scanContinuous = !scanContinuous;
    saveSettingsAfterLocalChange();
    Serial.print(F("[SCAN] Continuous = "));
    Serial.println(scanContinuous ? F("ON") : F("OFF"));
    printCfgStatus();
    return;
  }

  if (cmd == "L") {
    scanUsePieceLetters = !scanUsePieceLetters;
    saveSettingsAfterLocalChange();
    Serial.print(F("[SCAN] Letter view = "));
    Serial.println(scanUsePieceLetters ? F("ON") : F("OFF"));
    printCfgStatus();
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

  String response;
  bool handled = false;
  handleCommonCommand(cmd, cmd, response, handled);
  if (handled) {
    if (response.length() > 0) {
      bool isErr = response.startsWith("WIFI_ERR") || response.startsWith("CFG_ERR") || response.startsWith("LICHESS_ERR");
      if (isErr) {
        if (cmd.startsWith("WIFI")) {
          Serial.print(F("[WIFI] ERR "));
        } else if (cmd.startsWith("CFG")) {
          Serial.print(F("[CFG] ERR "));
        } else if (cmd.startsWith("LICHESS")) {
          Serial.print(F("[LICHESS] ERR "));
        } else {
          Serial.print(F("[CMD] ERR "));
        }
      } else {
        if (cmd.startsWith("WIFI")) {
          Serial.print(F("[WIFI] "));
        } else if (cmd.startsWith("CFG")) {
          Serial.print(F("[CFG] "));
        } else if (cmd.startsWith("LICHESS")) {
          Serial.print(F("[LICHESS] "));
        } else {
          Serial.print(F("[CMD] "));
        }
      }
      Serial.println(response);
    }
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

  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);

  initRfidScanner(mfrc522);
  bleFenBegin();
  bleFenSetCommandHandler(handleBleCommand);

  settingsStoreReady = settingsStore.begin(SETTINGS_NS, false);
  if (settingsStoreReady) {
    loadSettingsFromStore();
    loadWifiSettingsFromStore();
    Serial.print(F("[CFG] Loaded "));
    Serial.println(buildSettingsPayload("CFG"));
    Serial.print(F("[WIFI] Loaded "));
    Serial.println(buildWifiPayload("WIFI"));

    if (wifiAutoConnect && wifiSavedSsid.length() > 0) {
      startWifiConnect();
    }
  } else {
    Serial.println(F("[CFG] Preferences init failed, using defaults"));
  }

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

  wl_status_t wifiStatus = WiFi.status();
  bool wifiConnectedNow = (wifiStatus == WL_CONNECTED);

  if (wifiConnectInProgress) {
    if (wifiConnectedNow) {
      wifiConnectInProgress = false;
      wifiConnectFailed = false;
      wifiLastError = "";
      bleFenLog(String("[WIFI] CONNECTED ip=") + WiFi.localIP().toString());
    } else if (wifiStatus == WL_CONNECT_FAILED) {
      wifiConnectInProgress = false;
      wifiConnectFailed = true;
      wifiLastError = "AUTH";
      bleFenLog(String("[WIFI] CONNECT_FAILED auth ssid=") + wifiSavedSsid);
    } else if (wifiStatus == WL_NO_SSID_AVAIL) {
      wifiConnectInProgress = false;
      wifiConnectFailed = true;
      wifiLastError = "NO_SSID";
      bleFenLog(String("[WIFI] CONNECT_FAILED no_ssid ssid=") + wifiSavedSsid);
    } else if (millis() - wifiConnectStartMs > WIFI_CONNECT_TIMEOUT_MS) {
      wifiConnectInProgress = false;
      wifiConnectFailed = true;
      wifiLastError = "TIMEOUT";
      bleFenLog(String("[WIFI] CONNECT_TIMEOUT ssid=") + wifiSavedSsid);
    }
  }

  if (wifiConnectedNow != wifiLastConnected) {
    wifiLastConnected = wifiConnectedNow;
    if (!wifiConnectedNow && wifiAutoConnect && wifiSavedSsid.length() > 0) {
      startWifiConnect();
    }
  }

  processLichessUploadTick();

  if (!gameStarted) {
    esp_task_wdt_reset();
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

  esp_task_wdt_reset();
  delay(1);
}
