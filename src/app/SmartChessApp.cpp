#include "app/SmartChessApp.h"

#include <Arduino.h>
#include <esp32-hal-rgb-led.h>
#include <esp_task_wdt.h>
#include <MFRC522.h>
#include <Preferences.h>
#include <WiFi.h>

#include "chess/BoardState.h"
#include "ble/BleService.h"
#include "chess/FenBuilder.h"
#include "net/LichessPublish.h"
#include "chess/MoveGen.h"
#include "hardware/RfidScanner.h"
#include "utils/ScanDebug.h"
#include "hardware/BoardConfig.h"
#include "app/AppTypes.h"
#include "utils/TextUtils.h"
#include "net/WebPublish.h"
#include "net/BoardRegistration.h"

#include <esp_ota_ops.h>
#include <esp_system.h>
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

// SCAN_WAIT_RESTORE timers
static unsigned long waitRestoreNextScanMs  = 0;
static unsigned long waitRestoreNextPrintMs = 0;

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


static unsigned long gameStartTimeMs = 0;
static int totalMovesCount = 0;
// Set to true after a move is queued for HTTP delivery; cleared once the server
// confirms (2xx) or rejects (4xx).  On 4xx, totalMovesCount is decremented to
// keep the seq counter in sync with the server.
static bool webMovePendingConfirm = false;

// Snapshot of the game state saved just before applyMove() is called.
// Used to undo the local board model when the server rejects the move (4xx).
// savedRollbackFrom == -1 means no snapshot is held.
static int    savedRollbackFrom    = -1;
static int    savedRollbackTo      = -1;
static char   savedRollbackPiece   = '.';   // piece type that was at fromIdx
static char   savedRollbackTarget  = '.';   // piece type that was at toIdx (captured)
static String savedRollbackUID     = "";    // UID of the moved piece
static String savedRollbackTgtUID  = "";    // UID of the captured piece (or "")
static bool   savedRollbackWTurn   = true;
static int    savedRollbackHalf    = 0;
static int    savedRollbackFull    = 1;
static int    savedRollbackEPFile  = -1;
static int    savedRollbackEPRank  = -1;

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

// WebMovePublish / BoardRegistration NVS keys
constexpr const char *KEY_WEB_URL     = "web_url";
constexpr const char *KEY_WEB_GAME    = "web_game";
constexpr const char *KEY_WEB_ENABLED = "web_enabled";
constexpr const char *KEY_BOARD_ID    = "board_id";

static String webServerUrl = "";
static String webGameID    = "";
static bool   webEnabled   = false;
static String boardID      = "";

// WiFi-initiated game start (set by onBoardRegCommandReceived, executed in tick)
static bool pendingWifiStart = false;

// OTA-over-BLE state
static esp_ota_handle_t             gOtaHandle      = 0;
static const esp_partition_t       *gOtaPartition   = nullptr;
static bool                         gOtaInProgress  = false;
static size_t                       gOtaBytesWritten = 0;

static String wifiSavedSsid = "";
static String wifiSavedPass = "";
static bool wifiAutoConnect = true;
static bool wifiConnectInProgress = false;
static unsigned long wifiConnectStartMs = 0;
static bool wifiLastConnected = false;
static unsigned long wifiRetryScheduledMs = 0;   // 0 = no retry pending
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr unsigned long WIFI_RETRY_DELAY_MS     = 30000;  // retry 30 s after failure
constexpr int WIFI_SCAN_MAX = 24;
static String wifiScanResults[WIFI_SCAN_MAX];
static int    wifiScanRssi[WIFI_SCAN_MAX];
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

enum LedBaseState {
  LED_BASE_BOOT = 0,
  LED_BASE_IDLE_DISCONNECTED,
  LED_BASE_IDLE_CONNECTED,
  LED_BASE_GAME_READY,
  LED_BASE_GAME_TRACKING,
  LED_BASE_WIFI_CONNECTING,
  LED_BASE_WIFI_ERROR,
};

static LedBaseState ledBaseState = LED_BASE_BOOT;
static bool ledInitialized = false;
static unsigned long ledPulseUntilMs = 0;
static uint8_t ledPulseR = 0;
static uint8_t ledPulseG = 0;
static uint8_t ledPulseB = 0;

static bool startWifiConnect();
static bool handleWifiSetCommand(const String &cmd, String &response);
static const char* buildLichessPayload(const char* prefix);
static bool queueLichessPublish(String &response);
static void printCfgStatus();
static void printWifiStatus();
static const char* scanStateName();
static void printFullStatus();
static void abortTrackingRestoreSource();

static void setRgbNow(uint8_t r, uint8_t g, uint8_t b) {
#if defined(RGB_BUILTIN)
  neopixelWrite(RGB_BUILTIN, r, g, b);
#else
  (void)r;
  (void)g;
  (void)b;
#endif
}

static void pulseLed(uint8_t r, uint8_t g, uint8_t b, unsigned long holdMs) {
  ledPulseR = r;
  ledPulseG = g;
  ledPulseB = b;
  ledPulseUntilMs = millis() + holdMs;
}

static void setLedBaseState(LedBaseState st) {
  ledBaseState = st;
}

static void updateLedStatus() {
  unsigned long now = millis();

  if (!ledInitialized) {
    ledInitialized = true;
  }

  if (now < ledPulseUntilMs) {
    setRgbNow(ledPulseR, ledPulseG, ledPulseB);
    return;
  }

  switch (ledBaseState) {
    case LED_BASE_BOOT: {
      bool on = ((now / 180) % 2) == 0;
      if (on) {
        setRgbNow(0, 0, 20);
      } else {
        setRgbNow(0, 0, 0);
      }
      break;
    }
    case LED_BASE_IDLE_DISCONNECTED:
      setRgbNow(0, 0, 6);
      break;
    case LED_BASE_IDLE_CONNECTED:
      setRgbNow(0, 14, 14);
      break;
    case LED_BASE_GAME_READY:
      setRgbNow(0, 18, 0);
      break;
    case LED_BASE_GAME_TRACKING: {
      bool on = ((now / 120) % 2) == 0;
      if (on) {
        setRgbNow(6, 24, 0);
      } else {
        setRgbNow(0, 8, 0);
      }
      break;
    }
    case LED_BASE_WIFI_CONNECTING: {
      bool on = ((now / 260) % 2) == 0;
      if (on) {
        setRgbNow(24, 10, 0);
      } else {
        setRgbNow(3, 1, 0);
      }
      break;
    }
    case LED_BASE_WIFI_ERROR: {
      bool on = ((now / 180) % 2) == 0;
      if (on) {
        setRgbNow(24, 0, 0);
      } else {
        setRgbNow(0, 0, 0);
      }
      break;
    }
  }
}

static void pulseLedError() {
  pulseLed(40, 0, 0, 520);
}

static void pulseLedWarn() {
  pulseLed(38, 14, 0, 360);
}

static void pulseLedOk() {
  pulseLed(0, 42, 0, 260);
}

static void pulseLedInfo() {
  pulseLed(0, 18, 28, 220);
}

static void refreshLedBaseState() {
  if (wifiConnectInProgress) {
    setLedBaseState(LED_BASE_WIFI_CONNECTING);
    return;
  }

  if (wifiConnectFailed) {
    setLedBaseState(LED_BASE_WIFI_ERROR);
    return;
  }

  if (gameStarted) {
    if (scanState == SCAN_IDLE) {
      setLedBaseState(LED_BASE_GAME_READY);
    } else {
      setLedBaseState(LED_BASE_GAME_TRACKING);
    }
    return;
  }

  if (bleIsConnected()) {
    setLedBaseState(LED_BASE_IDLE_CONNECTED);
  } else {
    setLedBaseState(LED_BASE_IDLE_DISCONNECTED);
  }
}

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
    wifiScanRssi[i]    = (int)WiFi.RSSI(i);
  }
  for (int i = wifiScanCount; i < WIFI_SCAN_MAX; i++) {
    wifiScanResults[i] = "";
    wifiScanRssi[i]    = 0;
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
    payload += "|rssi=";
    payload += String(wifiScanRssi[idx]);
  }
  return payload;
}

static char wifiPayloadBuf[192];

static const char* buildWifiPayload(const char* prefix) {
  String mac = WiFi.macAddress(); // always available, e.g. "AA:BB:CC:DD:EE:FF"
  snprintf(wifiPayloadBuf, sizeof(wifiPayloadBuf),
    "%s|ver=%lu|ssid=%s|auto=%d|status=%s|ip=%s|mac=%s|err=%s",
    prefix, wifiSettingsVersion,
    wifiSavedSsid.c_str(),
    wifiAutoConnect ? 1 : 0,
    wifiStatusText().c_str(),
    wifiIpText().c_str(),
    mac.c_str(),
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

// ---------------------------------------------------------------------------
// WebMovePublish NVS helpers
// ---------------------------------------------------------------------------

// Wrapper that appends |board= (stored in this file) to the WebPublish payload.
static String buildWebPayload(const char* prefix) {
  String s = webPublishStatusPayload(prefix);
  s += "|board=";
  s += boardID.length() > 0 ? boardID : "-";
  return s;
}

static void persistWebSettingsToStore() {
  if (!settingsStoreReady) return;
  settingsStore.putString(KEY_WEB_URL, webServerUrl);
  settingsStore.putString(KEY_WEB_GAME, webGameID);
  settingsStore.putBool(KEY_WEB_ENABLED, webEnabled);
  settingsStore.putString(KEY_BOARD_ID, boardID);
}

static void loadWebSettingsFromStore() {
  if (!settingsStoreReady) return;
  webServerUrl = settingsStore.getString(KEY_WEB_URL, "");
  webGameID    = settingsStore.getString(KEY_WEB_GAME, "");
  webEnabled   = settingsStore.getBool(KEY_WEB_ENABLED, false);
  boardID      = settingsStore.getString(KEY_BOARD_ID, "");
}

static bool handleWebSetCommand(const String &cmd, String &response) {
  bool hasAnyField = false;

  String vUrl     = settingsFieldValue(cmd, "URL");
  String vGame    = settingsFieldValue(cmd, "GAME");
  String vEnabled = settingsFieldValue(cmd, "ENABLED");
  String vBoard   = settingsFieldValue(cmd, "BOARD");

  if (vUrl.length() > 0)   { webServerUrl = vUrl;  hasAnyField = true; }
  if (vGame.length() > 0)  { webGameID    = vGame; hasAnyField = true; }
  if (vBoard.length() > 0) { boardID      = vBoard; hasAnyField = true; }
  if (vEnabled.length() > 0) {
    bool b = false;
    if (!parseBoolToken(vEnabled, b)) {
      response = "WEB_ERR:BAD_ENABLED";
      return false;
    }
    webEnabled  = b;
    hasAnyField = true;
  }

  if (!hasAnyField) {
    response = "WEB_ERR:NO_FIELDS";
    return false;
  }

  // Apply to runtime modules (single call each, covers all changed fields above)
  webPublishSetConfig(webServerUrl, webGameID, webEnabled);
  boardRegSetConfig(webServerUrl, boardID);
  persistWebSettingsToStore();

  // Trigger an immediate heartbeat so the board gets its gameID without
  // waiting up to 30 s after config is saved.
  boardRegForceHeartbeat();

  response = buildWebPayload("WEB_SAVED");
  return true;
}

// ---------------------------------------------------------------------------
// BoardRegistration callback ? called when server assigns a new gameID
// ---------------------------------------------------------------------------

static void onBoardRegGameIDAssigned(const String &newGameID) {
  webGameID = newGameID;
  webPublishSetConfig(webServerUrl, webGameID, webEnabled);
  persistWebSettingsToStore();
  // doHeartbeat() already prints the Serial line; send via BLE terminal only.
  bleLog(String(F("[REG] gameID assigned=")) + newGameID);
}

// Called from BoardRegistration when heartbeat response contains "command" field.
// Runs inside HTTP callback context — only set a flag, execute in main tick.
static void onBoardRegCommandReceived(const String &command) {
  String cmdUpper = command;
  cmdUpper.toUpperCase();
  if (cmdUpper == "START") {
    pendingWifiStart = true;
    bleLog(F("[CMD] WiFi START received — scanning board..."));
  } else if (cmdUpper == "STOP") {
    // Honoured immediately: safe, just sets a flag
    if (gameStarted) {
      gameStarted = false;
      abortTrackingRestoreSource();
      bleLog(F("[CMD] WiFi STOP received — game stopped"));
    }
  } else {
    bleLog(String(F("[WARN] Unknown WiFi command: ")) + command);
  }
}

// ---------------------------------------------------------------------------
// OTA-over-BLE handler ? called from BLE OTA characteristic onWrite
// ---------------------------------------------------------------------------

static void onBleOtaData(const uint8_t *data, size_t len) {
  if (len == 0) return;

  // OTA_BEGIN:<total_size>
  if (len >= 9 && memcmp(data, "OTA_BEGIN", 9) == 0) {
    gOtaPartition = esp_ota_get_next_update_partition(nullptr);
    if (!gOtaPartition) {
      bleOtaRespond("OTA_ERR:NO_PARTITION");
      Serial.println(F("[OTA] No OTA partition found"));
      return;
    }
    esp_err_t err = esp_ota_begin(gOtaPartition, OTA_SIZE_UNKNOWN, &gOtaHandle);
    if (err != ESP_OK) {
      bleOtaRespond("OTA_ERR:BEGIN_FAILED");
      Serial.printf("[OTA] esp_ota_begin error: %d\n", err);
      return;
    }
    gOtaInProgress   = true;
    gOtaBytesWritten = 0;
    bleOtaRespond("OTA_READY");
    Serial.println(F("[OTA] Started"));
    return;
  }

  // OTA_END
  if (len == 7 && memcmp(data, "OTA_END", 7) == 0) {
    if (!gOtaInProgress) {
      bleOtaRespond("OTA_ERR:NOT_STARTED");
      return;
    }
    gOtaInProgress = false;
    esp_err_t err = esp_ota_end(gOtaHandle);
    if (err != ESP_OK) {
      bleOtaRespond("OTA_ERR:END_FAILED");
      Serial.printf("[OTA] esp_ota_end error: %d\n", err);
      return;
    }
    err = esp_ota_set_boot_partition(gOtaPartition);
    if (err != ESP_OK) {
      bleOtaRespond("OTA_ERR:SET_BOOT_FAILED");
      return;
    }
    bleOtaRespond("OTA_OK");
    Serial.printf("[OTA] Success! %zu bytes written. Restarting...\n", gOtaBytesWritten);
    delay(500);
    esp_restart();
    return;
  }

  // Binary chunk
  if (gOtaInProgress) {
    esp_err_t err = esp_ota_write(gOtaHandle, data, len);
    if (err != ESP_OK) {
      gOtaInProgress = false;
      bleOtaRespond("OTA_ERR:WRITE_FAILED");
      Serial.printf("[OTA] esp_ota_write error: %d\n", err);
      return;
    }
    gOtaBytesWritten += len;
    char ack[40];
    snprintf(ack, sizeof(ack), "OTA_ACK:%zu", gOtaBytesWritten);
    bleOtaRespond(ack);
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
  pulseLedInfo();
  bleLog(String("[WIFI] CONNECTING ssid=") + wifiSavedSsid);
  return true;
}

static void disconnectWifiNow() {
  WiFi.disconnect(true);
  wifiConnectInProgress = false;
  wifiConnectFailed = false;
  wifiLastError = "";
  wifiConnectStartMs = 0;
  wifiLastConnected = false;
  wifiRetryScheduledMs = 0;   // cancel any pending auto-retry
  pulseLedInfo();
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
  bleLog(String("[CFG] SAVED ") + response);
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
    // Push each network via LOG immediately so web receives them before CMD ACK
    for (int i = 0; i < wifiScanCount; i++) {
      String net = String("[WIFI_NET] idx=") + i
                 + "|ssid=" + wifiScanResults[i]
                 + "|rssi=" + String(wifiScanRssi[i]);
      bleLogImmediate(net);
    }
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

  // WebMovePublish commands
  if (cmdUpper == "WEB" || cmdUpper == "WEB?" || cmdUpper == "WEB_GET") {
    handled = true;
    response = buildWebPayload("WEB");
    return true;
  }

  if (cmdUpper.startsWith("WEB_SET")) {
    handled = true;
    return handleWebSetCommand(cmd, response);
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
  bleLog(String("[WIFI] SAVED ") + response);
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
  fenBuild(board,
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
  return lichessQueuePublish(ctx, response, bleLog);
}

static void requestLichessAutoPublish() {
  LichessContext ctx = makeLichessContext();
  lichessRequestAutoPublish(ctx, bleLog);
}

static void processLichessUploadTick() {
  LichessContext ctx = makeLichessContext();
  lichessProcessUploadTick(ctx, bleLog);
}

static void printFEN() {
  Serial.print(F("[FEN] "));
  Serial.println(buildCurrentFen());
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

  // Clear scanBuf for start squares before scanning
  for (int i = 0; i < NUM_ANTENNAS; i++) {
    if (startMask[i]) scanBuf[i] = "";
  }

  // Multi-pass scan: retry only missing squares (up to 3 passes)
  constexpr int  START_SCAN_PASSES   = 3;
  constexpr int  START_PASS_DELAY_MS = 50;
  bool remainMask[NUM_ANTENNAS];
  memcpy(remainMask, startMask, sizeof(remainMask));
  String passBuf[NUM_ANTENNAS];

  for (int pass = 0; pass < START_SCAN_PASSES; pass++) {
    if (pass > 0) delay(START_PASS_DELAY_MS);
    for (int i = 0; i < NUM_ANTENNAS; i++) passBuf[i] = "";
    selectiveScanToBuffer(mfrc522, remainMask, passBuf,
                          lastScannedCount, lastFullScanUs,
                          lastAvgCellUs, lastMinCellUs, lastMaxCellUs);

    int stillMissing = 0;
    for (int i = 0; i < NUM_ANTENNAS; i++) {
      if (!startMask[i]) continue;
      if (passBuf[i].length() > 0) {
        scanBuf[i]    = passBuf[i];
        remainMask[i] = false;
      } else if (remainMask[i]) {
        stillMissing++;
      }
    }
    Serial.print(F("[START] Pass "));
    Serial.print(pass + 1);
    Serial.print(F(": "));
    Serial.print(32 - stillMissing);
    Serial.println(F("/32"));
    if (stillMissing == 0) break;
  }

  int foundCount = 0;
  for (int i = 0; i < NUM_ANTENNAS; i++) {
    if (startMask[i] && scanBuf[i].length() > 0) foundCount++;
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
    startFailReason = "MISSING";
    startFailDetail = missingSquares;
    bleLog(String("[ERR] MISSING_SQUARES ") + missingSquares + String(" | place piece and START again"));
    return false;
  }

  if (!validateNoDuplicateUids(startMask)) {
    gameStarted = false;
    startFailReason = "DUPLICATE";
    if (startFailDetail.length() == 0) {
      startFailDetail = "unknown";
    }
    bleLog(String("[ERR] DUPLICATE_UID ") + startFailDetail + String(" | fix tags and START again"));
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
  webMovePendingConfirm = false;
  savedRollbackFrom     = -1;

  Serial.println(F("[START] Du 32 quan. Bat dau tracking."));
  bleLog(String("[READY] STARTED | pieces=32"));
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

static bool timedScanPresent(int idx) {
  uint32_t st = micros();
  bool present = scanPresent(mfrc522, idx);
  uint32_t dt = micros() - st;

  cycleScanCount++;
  cycleScanSumUs += dt;
  if (dt < cycleScanMinUs) {
    cycleScanMinUs = dt;
  }
  if (dt > cycleScanMaxUs) {
    cycleScanMaxUs = dt;
  }
  return present;
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
      bleLog(String("[ERR] UNKNOWN_PIECE uid=") + movedUid + String(" | reset piece then retry"));
      return false;
    }
  }

  bool movingWhite = isWhitePiece(piece);
  if ((whiteTurn && !movingWhite) || (!whiteTurn && movingWhite)) {
    Serial.println(F("[TURN] Wrong side moved."));
    bleLog(String("[ERR] WRONG_TURN expected=") + sideName(whiteTurn) +
              String(" got=") + sideName(movingWhite) +
              String(" at ") + squareNameFromIdx(fromIdx) +
              String(" | place piece back"));
    pulseLedError();
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
    bleLog(String("[WARN] LICHESS_APPEND_FAILED move=") + lichessSanMove + String(" raw=") + moveText);
    pulseLedWarn();
  }

  requestLichessAutoPublish();

  Serial.print(F("[MOVE] "));
  Serial.println(moveText);
  bleLog(String("[MOVE] ") + moveText + String(" | ") + squareNameFromIdx(fromIdx) + String("->") + squareNameFromIdx(toIdx));
  pulseLedOk();
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
    bool present = timedScanPresent(idx);

    if (!present && squareUID[idx].length() > 0) {
      setPendingLift(idx, squareUID[idx]);
      return;
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
  bleLog(String("[LIFT] ") + squareNameFromIdx(liftedFromIdx) + String(" piece lifted"));

  scanState = SCAN_PIECE_LIFTED;
}

static void handlePieceLifted() {
  if (millis() - liftDetectedMs < DEBOUNCE_LIFT_MS) {
    return;
  }

  String recheck = timedScanSquare(liftedFromIdx);
  if (recheck == liftedUID) {
    Serial.println(F("[LIFT] false-lift"));
    bleLog(String("[INFO] LIFT_GLITCH at ") + squareNameFromIdx(liftedFromIdx) + String(" | scan glitch ignored"));
    pulseLedWarn();
    abortTrackingRestoreSource();
    return;
  }

  bool liftedIsWhite = isWhitePiece(liftedPiece);
  if ((whiteTurn && !liftedIsWhite) || (!whiteTurn && liftedIsWhite)) {
    Serial.println(F("[TRACK] wrong turn -> fallback"));
    String alertDetail = String("expected=") + sideName(whiteTurn)
                       + String(" got=") + sideName(liftedIsWhite)
                       + String(" at ") + squareNameFromIdx(liftedFromIdx);
    bleLog(String("[ERR] WRONG_TURN ") + alertDetail + String(" | place piece back"));
    boardRegQueueAlert("WRONG_TURN", alertDetail);
    pulseLedError();
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
    String alertDetail = String("from=") + squareNameFromIdx(liftedFromIdx);
    bleLog(String("[ERR] NO_LEGAL_DEST ") + alertDetail + String(" | reset piece then retry"));
    boardRegQueueAlert("NO_LEGAL_DEST", alertDetail);
    pulseLedError();
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
    bleLog(String("[INFO] RETURNED_SOURCE ") + squareNameFromIdx(liftedFromIdx));
    pulseLedInfo();
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
      bleLog(String("[TRACK] TARGET ") + squareNameFromIdx(liftedToIdx));
      pulseLedInfo();
      scanState = SCAN_VERIFY;
      return;
    }
  }

  if (millis() - trackingStartMs > TRACKING_TIMEOUT_MS) {
    Serial.println(F("[TRACK] timeout -> fallback"));
    String alertDetail = String("from=") + squareNameFromIdx(liftedFromIdx);
    bleLog(String("[ERR] TRACK_TIMEOUT ") + alertDetail + String(" | place piece correctly"));
    boardRegQueueAlert("TRACK_TIMEOUT", alertDetail);
    pulseLedError();
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
    // Reject moves to illegal squares — happens when FALLBACK found piece
    // outside the legal candidate list (e.g. piece bumped to wrong square).
    if (!isIdxInCandidates(liftedToIdx)) {
      Serial.print(F("[VERIFY] ILLEGAL_DEST "));
      Serial.println(squareNameFromIdx(liftedToIdx));
      String alertDetail = String("from=") + squareNameFromIdx(liftedFromIdx)
                         + String(" to=") + squareNameFromIdx(liftedToIdx);
      bleLog(String(F("[ERR] ILLEGAL_DEST ")) + alertDetail + F(" | move is not legal — place piece on a valid square"));
      boardRegQueueAlert("ILLEGAL_DEST", alertDetail);
      pulseLedError();
      verifyCount = 0;
      fallbackStartMs = millis();
      lastFallbackTryMs = 0;
      scanState = SCAN_FALLBACK;
      return;
    }

    // Snapshot board state BEFORE applyMove so we can undo it if the server
    // rejects the move as illegal (HTTP 4xx → SERVER_ILLEGAL flow).
    {
      int ff = liftedFromIdx / 8, fr = liftedFromIdx % 8;
      int tf = liftedToIdx   / 8, tr = liftedToIdx   % 8;
      savedRollbackFrom   = liftedFromIdx;
      savedRollbackTo     = liftedToIdx;
      savedRollbackUID    = liftedUID;
      savedRollbackPiece  = board[ff][fr];
      savedRollbackTarget = board[tf][tr];
      savedRollbackTgtUID = squareUID[liftedToIdx];
      savedRollbackWTurn  = whiteTurn;
      savedRollbackHalf   = halfmoveClock;
      savedRollbackFull   = fullmoveNumber;
      savedRollbackEPFile = enPassantFile;
      savedRollbackEPRank = enPassantRank;
    }

    if (applyMove(liftedFromIdx, liftedToIdx, liftedUID)) {
      totalMovesCount++;

      // Build UCI and forward to BEChessWeb server
      String uci = String(squareNameFromIdx(liftedFromIdx)) + String(squareNameFromIdx(liftedToIdx));
      int toRank = liftedToIdx % 8;
      if ((liftedPiece == 'P' && toRank == 7) || (liftedPiece == 'p' && toRank == 0)) {
        uci += 'q';  // auto-promote to queen
      }
      webPublishMove(uci, totalMovesCount);
      webMovePendingConfirm = true;   // cleared by main-loop check after poll

      resetTrackingState();
      bleLog(String(F("[OK] MOVE_CONFIRMED uci=")) + uci);
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
    // Do one last full-board scan to confirm piece is truly missing
    String srcCheck = timedScanSquare(liftedFromIdx);
    bool foundAnywhere = (srcCheck == liftedUID);
    if (!foundAnywhere) {
      // Check all occupied squares one more time
      int dummy = -1;
      foundAnywhere = findLiftedUidInList(occupiedList, occupiedCount, -1, dummy);
    }

    if (!foundAnywhere) {
      // Piece is physically gone — removed from board or RFID tag failed.
      // Do NOT corrupt the board model. Instead enter SCAN_WAIT_RESTORE so the
      // player can put the piece back and the game resumes automatically.
      Serial.print(F("[FALLBACK] PIECE_LOST uid="));
      Serial.print(liftedUID);
      Serial.print(F(" from "));
      Serial.println(squareNameFromIdx(liftedFromIdx));
      String alertDetail = String("from=") + squareNameFromIdx(liftedFromIdx)
                         + String(" uid=") + liftedUID;
      bleLog(String(F("[ERR] PIECE_LOST ")) + alertDetail
             + F(" | place piece back on board — game will resume automatically"));
      boardRegQueueAlert("PIECE_LOST", alertDetail);
      pulseLedError();
      // Enter restore mode — do NOT call abortTrackingRestoreSource() here.
      scanState = SCAN_WAIT_RESTORE;
      waitRestoreNextScanMs  = 0;
      waitRestoreNextPrintMs = 0;
      Serial.println(F("[RESTORE] Waiting for piece to be placed back..."));
      printBoard(board);
      return;
    } else {
      Serial.println(F("[FALLBACK] timeout, piece found — aborting track"));
      String alertDetail = String("from=") + squareNameFromIdx(liftedFromIdx);
      bleLog(String(F("[ERR] FALLBACK_TIMEOUT ")) + alertDetail + F(" | place piece on a legal square"));
      boardRegQueueAlert("FALLBACK_TIMEOUT", alertDetail);
      pulseLedError();
    }
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
    bleLog(String("[INFO] FALLBACK_RETURN_SOURCE"));
    pulseLedInfo();
    abortTrackingRestoreSource();
    return;
  }

  int foundIdx = -1;
  if (findLiftedUidInList(candidateList, candidateCount, liftedFromIdx, foundIdx)) {
    liftedToIdx = foundIdx;
    verifyCount = 0;
    Serial.print(F("[FALLBACK] found in candidates: "));
    Serial.println(squareNameFromIdx(liftedToIdx));
    bleLog(String("[FALLBACK] FOUND_CANDIDATE ") + squareNameFromIdx(liftedToIdx));
    pulseLedInfo();
    scanState = SCAN_VERIFY;
    return;
  }

  if (findLiftedUidInList(occupiedList, occupiedCount, liftedFromIdx, foundIdx)) {
    if (!isIdxInCandidates(foundIdx)) {
      // Piece confirmed at an illegal square. Fire ILLEGAL_DEST alert directly
      // and stay in SCAN_FALLBACK. Going to SCAN_VERIFY here would create an
      // infinite FALLBACK → VERIFY → ILLEGAL_DEST → FALLBACK loop.
      Serial.print(F("[FALLBACK] found outside candidates: "));
      Serial.println(squareNameFromIdx(foundIdx));
      bleLog(String("[WARN] FOUND_OUTSIDE_CANDIDATE ") + squareNameFromIdx(foundIdx) + String(" | place piece on a legal square"));
      pulseLedWarn();
      String alertDetail = String("from=") + squareNameFromIdx(liftedFromIdx)
                         + String(" to=") + squareNameFromIdx(foundIdx);
      boardRegQueueAlert("ILLEGAL_DEST", alertDetail);
      // Keep scanning in FALLBACK — timeout will abort if user never fixes it
      return;
    }
    liftedToIdx = foundIdx;
    verifyCount = 0;
    scanState = SCAN_VERIFY;
  }
}

// ---------------------------------------------------------------------------
// SCAN_WAIT_RESTORE
// Board model is intact. A piece is missing from the physical board.
// We wait — without time limit — for:
//   a) The piece to return to liftedFromIdx (game state unchanged, no move applied)
//   b) The piece to land on a legal candidate square (move confirmed → SCAN_VERIFY)
// The board position is reprinted every WAIT_RESTORE_PRINT_MS as a reminder.
// ---------------------------------------------------------------------------
static void handleWaitRestore() {
  unsigned long now = millis();

  // Periodically reprint the expected board so the player knows what to restore
  if (now >= waitRestoreNextPrintMs) {
    waitRestoreNextPrintMs = now + WAIT_RESTORE_PRINT_MS;
    Serial.println(F("[RESTORE] Expected board — restore missing piece:"));
    printBoard(board);
    if (liftedFromIdx >= 0) {
      Serial.print(F("[RESTORE] Missing: "));
      Serial.print(squareNameFromIdx(liftedFromIdx));
      Serial.print(F(" ("));
      Serial.print(liftedPiece);
      Serial.println(F(") — place it back"));
    }
  }

  if (now < waitRestoreNextScanMs) return;
  waitRestoreNextScanMs = now + WAIT_RESTORE_SCAN_MS;

  if (liftedFromIdx < 0 || liftedUID.length() == 0) {
    // Shouldn't happen — if we somehow ended up here without a tracked piece,
    // just reset cleanly so the game can continue.
    resetTrackingState();  // sets scanState = SCAN_IDLE
    return;
  }

  // Case A: piece returned to its original square — no move made, game continues
  String srcNow = timedScanSquare(liftedFromIdx);
  if (srcNow == liftedUID) {
    Serial.print(F("[RESTORE] Piece returned to "));
    Serial.println(squareNameFromIdx(liftedFromIdx));
    bleLog(String(F("[RESTORE] OK piece back at ")) + squareNameFromIdx(liftedFromIdx)
           + F(" | game continues"));
    pulseLedOk();
    // Restore UID tracking (squareUID[liftedFromIdx] may have been cleared in
    // some code paths before PIECE_LOST; ensure it is set correctly).
    squareUID[liftedFromIdx] = liftedUID;
    rebuildOccupiedListFromBoard();
    resetTrackingState();  // → SCAN_IDLE
    // Notify server/web that the piece is back and the game has resumed.
    boardRegQueueAlert("RESTORE_OK", String("at=") + squareNameFromIdx(liftedFromIdx));
    return;
  }

  // Case B: piece landed on a legal candidate (player chose to make the move)
  if (candidateCount > 0) {
    int foundIdx = -1;
    if (findLiftedUidInList(candidateList, candidateCount, liftedFromIdx, foundIdx)) {
      Serial.print(F("[RESTORE] Piece at candidate "));
      Serial.println(squareNameFromIdx(foundIdx));
      bleLog(String(F("[RESTORE] Piece at ")) + squareNameFromIdx(foundIdx)
             + F(" | verifying move"));
      pulseLedInfo();
      liftedToIdx  = foundIdx;
      verifyCount  = 0;
      scanState    = SCAN_VERIFY;
    }
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
    case SCAN_WAIT_RESTORE:
      handleWaitRestore();
      break;
  }
}

static void printHelp() {
  Serial.println(F("=================================================="));
  Serial.println(F("  SmartChess — COMMANDS"));
  Serial.println(F("=================================================="));
  Serial.println(F("  Game control"));
  Serial.println(F("    START                       start / re-learn board"));
  Serial.println(F("    STOP                        stop game"));
  Serial.println(F("    STATUS                      full status table"));
  Serial.println(F("    F / FEN                     print current FEN"));
  Serial.println(F("  Scanning"));
  Serial.println(F("    S / SNAP                    64-square snapshot"));
  Serial.println(F("    V                           toggle verbose"));
  Serial.println(F("    C                           toggle continuous print"));
  Serial.println(F("    L                           toggle piece letters"));
  Serial.println(F("    T                           print scan timing"));
  Serial.println(F("  Config"));
  Serial.println(F("    CFG                         read config"));
  Serial.println(F("    CFG_SET|VERBOSE=0|CONTINUOUS=1|LETTERS=1"));
  Serial.println(F("  WiFi"));
  Serial.println(F("    WIFI                        read WiFi status + MAC"));
  Serial.println(F("    WIFI_SET|SSID=...|PASS=...|CONNECT=1"));
  Serial.println(F("    WIFI_SCAN                   scan networks (push via BLE)"));
  Serial.println(F("    WIFI_CONNECT / WIFI_DISCONNECT"));
  Serial.println(F("  Web Publish"));
  Serial.println(F("    WEB                         read web config"));
  Serial.println(F("    WEB_SET|URL=http://IP:8080|GAME=game_alpha|ENABLED=1"));
  Serial.println(F("  Lichess"));
  Serial.println(F("    LICHESS_PUBLISH / LICHESS_STATUS"));
  Serial.println(F("    LICHESS_STREAM_ON / LICHESS_STREAM_OFF"));
  Serial.println(F("  Misc"));
  Serial.println(F("    B                           BLE connection state"));
  Serial.println(F("    CLEAR                       clear serial screen"));
  Serial.println(F("    HELP                        this help"));
  Serial.println(F("=================================================="));
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

  // START via BLE — local override for dev/testing.
  // In production, game start is triggered by the server via heartbeat command.
  if (cmdUpper == "START") {
    if (startAndLearnInitial32()) {
      pulseLedOk();
      response = "STARTED";
      boardRegQueueScanResult("STARTED", "32");
      if (webEnabled && webGameID.length() == 0) {
        response += "|WARN:NO_GAME_ID";
        bleLog(F("[WARN] NO_GAME_ID — create a game on the web UI first"));
      }
      return true;
    }
    pulseLedError();
    boardRegQueueScanResult(startFailReason, startFailDetail);
    response = String(F("START_FAILED:")) + startFailReason;
    if (startFailDetail.length() > 0) response += String(F(":")) + startFailDetail;
    bleLog(String(F("[ERR] START_FAILED ")) + startFailReason
           + F(" | ") + (startFailDetail.length() ? startFailDetail : String(F("check board"))));
    return false;
  }

  if (cmdUpper == "STOP") {
    gameStarted = false;
    abortTrackingRestoreSource();
    pulseLedInfo();

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
    bleLog(String("[INFO] STOPPED moves=") + String(totalMovesCount) + String(" time=") + String(mins) + String("m") + String(secs) + String("s"));
    return true;
  }

  if (cmdUpper == "STATUS") {
    // Compact pipe-delimited payload so web can parse it
    String s = gameStarted ? F("RUNNING") : F("IDLE");
    s += F("|moves=");      s += totalMovesCount;
    s += F("|turn=");       s += (whiteTurn ? F("WHITE") : F("BLACK"));
    s += F("|scan=");       s += scanStateName();
    s += F("|occupied=");   s += occupiedCount;
    s += F("|ble=");        s += (bleIsConnected() ? 1 : 0);
    s += F("|wifi=");       s += wifiStatusText();
    if (WiFi.status() == WL_CONNECTED) {
      s += F("|ip=");       s += WiFi.localIP().toString();
    }
    s += F("|web=");        s += (webEnabled ? 1 : 0);
    s += F("|board=");      s += (boardID.length() > 0 ? boardID : String("-"));
    response = s;
    pulseLedInfo();
    printFullStatus(); // also dump to Serial for debug
    return true;
  }

  if (cmdUpper == "CLEAR") {
    Serial.print(F("\033[2J\033[H"));
    Serial.println(F("[SmartChess] Screen cleared."));
    response = "OK";
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
      pulseLedInfo();
    } else {
      response = "NO_GAME";
      pulseLedWarn();
    }
    if (!gameStarted) {
      bleLog(String("[WARN] NO_GAME | send START first"));
    }
    return gameStarted;
  }

  response = "UNKNOWN_CMD";
  bleLog(String("[ERR] UNKNOWN_CMD ") + cmdUpper);
  pulseLedError();
  return false;
}

static const char* scanStateName() {
  switch (scanState) {
    case SCAN_IDLE:                 return "IDLE";
    case SCAN_LIFT_PENDING:         return "LIFT_PENDING";
    case SCAN_PIECE_LIFTED:         return "PIECE_LIFTED";
    case SCAN_TRACKING_DESTINATION: return "TRACKING";
    case SCAN_VERIFY:               return "VERIFY";
    case SCAN_FALLBACK:             return "FALLBACK";
    case SCAN_WAIT_RESTORE:         return "WAIT_RESTORE";
    default:                        return "?";
  }
}

// ── ASCII table helpers ────────────────────────────────────────────
// Layout (48 chars wide):
//   +-----------------+----------------------------+
//   | key  (15 chars) | value (26 chars)           |
//   | span title (44 chars)                        |
// Divider:  1 + 17'-' + 1 + 28'-' + 1  = 48
// Data row: 1 + 1 + 15 + 1 + 1 + 1 + 26 + 1 + 1  = 48
// Span row: 1 + 1 + 44 + 1 + 1                    = 48

static void stDiv() {
  Serial.println(F("+-----------------+----------------------------+"));
}

// Row: | key             | value                      |
static void stRow(const __FlashStringHelper *key, const char *val) {
  Serial.print(F("| "));
  size_t klen = strlen_P(reinterpret_cast<const char *>(key));
  Serial.print(key);
  for (size_t i = klen; i < 15; i++) Serial.print(' ');
  Serial.print(F(" | "));
  size_t vlen = strlen(val);
  if (vlen <= 26) {
    Serial.print(val);
    for (size_t i = vlen; i < 26; i++) Serial.print(' ');
  } else {
    for (int i = 0; i < 25; i++) Serial.print(val[i]);
    Serial.print('>');   // truncation marker
  }
  Serial.println(F(" |"));
}

static void stRow(const __FlashStringHelper *key, const String &val) {
  stRow(key, val.c_str());
}

static void stRow(const __FlashStringHelper *key, int val) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%d", val);
  stRow(key, buf);
}

// Span row: | text (44 chars, full width)           |
static void stSpan(const __FlashStringHelper *text) {
  Serial.print(F("| "));
  size_t len = strlen_P(reinterpret_cast<const char *>(text));
  Serial.print(text);
  for (size_t i = len; i < 44; i++) Serial.print(' ');
  Serial.println(F(" |"));
}

static void stSpan(const char *text) {
  Serial.print(F("| "));
  size_t len = strlen(text);
  Serial.print(text);
  for (size_t i = len; i < 44; i++) Serial.print(' ');
  Serial.println(F(" |"));
}

static void printFullStatus() {
  stDiv();
  stSpan(F("SmartChess STATUS"));
  stDiv();

  // ── Game ──────────────────────────────────────────────────────────
  stSpan(F("[GAME]"));
  stDiv();
  stRow(F("state"),    gameStarted ? "RUNNING" : "IDLE");
  if (gameStarted) {
    stRow(F("turn"),   whiteTurn ? "WHITE" : "BLACK");
    stRow(F("moves"),  totalMovesCount);
    stRow(F("scan"),   scanStateName());
    unsigned long elapsed = (gameStartTimeMs > 0) ? (millis() - gameStartTimeMs) / 1000 : 0;
    char timeBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%lum %02lus", elapsed / 60, elapsed % 60);
    stRow(F("elapsed"), timeBuf);
  }
  stRow(F("occupied"), occupiedCount);

  // ── BLE ───────────────────────────────────────────────────────────
  stDiv();
  stSpan(F("[BLE]"));
  stDiv();
  stRow(F("status"), bleIsConnected() ? "CONNECTED" : "DISCONNECTED");

  // ── WiFi ──────────────────────────────────────────────────────────
  stDiv();
  stSpan(F("[WiFi]"));
  stDiv();
  stRow(F("status"), wifiStatusText());
  if (wifiSavedSsid.length() > 0) stRow(F("ssid"), wifiSavedSsid);
  if (WiFi.status() == WL_CONNECTED) {
    stRow(F("ip"),  WiFi.localIP().toString());
    stRow(F("mac"), WiFi.macAddress());
  }
  if (wifiLastError.length() > 0) stRow(F("err"), wifiLastError);

  // ── Web Publish ────────────────────────────────────────────────────
  stDiv();
  stSpan(F("[WEB]"));
  stDiv();
  stRow(F("enabled"), webEnabled ? "YES" : "NO");
  if (webServerUrl.length() > 0) stRow(F("url"),   webServerUrl);
  if (webGameID.length()   > 0) stRow(F("game"),  webGameID);
  if (boardID.length()     > 0) stRow(F("board"), boardID);

  // ── CFG ───────────────────────────────────────────────────────────
  stDiv();
  stSpan(F("[CFG]"));
  stDiv();
  stRow(F("verbose"),    scanVerbose         ? "1" : "0");
  stRow(F("continuous"), scanContinuous      ? "1" : "0");
  stRow(F("letters"),    scanUsePieceLetters ? "1" : "0");

  // ── FEN ───────────────────────────────────────────────────────────
  if (gameStarted) {
    stDiv();
    stSpan(F("[FEN]"));
    stDiv();
    stSpan(buildCurrentFen());
  }

  stDiv();
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

  if (cmd == "CLEAR") {
    // ANSI: clear screen + move cursor to home
    Serial.print(F("\033[2J\033[H"));
    Serial.println(F("[SmartChess] Screen cleared. Type HELP for commands."));
    return;
  }

  if (cmd == "START") {
    if (startAndLearnInitial32()) {
      pulseLedOk();
      Serial.println(F("[READY] Game started. Scan toi uu theo so quan hien co."));
    } else {
      pulseLedError();
      Serial.println(F("[START] Khong the bat dau. Kiem tra vi tri 32 quan roi START lai."));
    }
    return;
  }

  if (cmd == "STOP") {
    gameStarted = false;
    abortTrackingRestoreSource();
    pulseLedInfo();
    Serial.println(F("[STOP] Da dung game. Gui START de bat dau lai."));
    return;
  }

  if (cmd == "STATUS") {
    pulseLedInfo();
    printFullStatus();
    return;
  }

  if (cmd == "F" || cmd == "FEN") {
    if (!gameStarted) {
      pulseLedWarn();
      Serial.println(F("[FEN] Chua START."));
    } else {
      pulseLedInfo();
      printFEN();
    }
    return;
  }

  if (cmd == "S" || cmd == "SNAP") {
    pulseLedInfo();
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
    pulseLedInfo();
    Serial.print(F("[SCAN] Verbose = "));
    Serial.println(scanVerbose ? F("ON") : F("OFF"));
    printCfgStatus();
    return;
  }

  if (cmd == "C") {
    scanContinuous = !scanContinuous;
    saveSettingsAfterLocalChange();
    pulseLedInfo();
    Serial.print(F("[SCAN] Continuous = "));
    Serial.println(scanContinuous ? F("ON") : F("OFF"));
    printCfgStatus();
    return;
  }

  if (cmd == "L") {
    scanUsePieceLetters = !scanUsePieceLetters;
    saveSettingsAfterLocalChange();
    pulseLedInfo();
    Serial.print(F("[SCAN] Letter view = "));
    Serial.println(scanUsePieceLetters ? F("ON") : F("OFF"));
    printCfgStatus();
    return;
  }

  if (cmd == "T") {
    pulseLedInfo();
    printScanTiming(lastScannedCount, lastFullScanUs, lastAvgCellUs, lastMinCellUs, lastMaxCellUs);
    return;
  }

  if (cmd == "B") {
    pulseLedInfo();
    Serial.print(F("[BLE] connected="));
    Serial.println(bleIsConnected() ? F("YES") : F("NO"));
    return;
  }

  String response;
  bool handled = false;
  handleCommonCommand(cmd, cmd, response, handled);
  if (handled) {
    if (response.startsWith("WIFI_ERR") || response.startsWith("CFG_ERR") || response.startsWith("LICHESS_ERR")) {
      pulseLedError();
    } else {
      pulseLedInfo();
    }
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
  pulseLedError();
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

    if (cmdBuffer.length() < 256) {
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

#if defined(RGB_BUILTIN)
  pinMode(RGB_BUILTIN, OUTPUT);
#endif
  setLedBaseState(LED_BASE_BOOT);
  pulseLedInfo();
  updateLedStatus();

  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);

  initRfidScanner(mfrc522);
  bleServiceBegin();
  bleSetCommandHandler(handleBleCommand);
  bleSetOtaCallback(onBleOtaData);

  settingsStoreReady = settingsStore.begin(SETTINGS_NS, false);
  if (settingsStoreReady) {
    loadSettingsFromStore();
    loadWifiSettingsFromStore();
    loadWebSettingsFromStore();
    Serial.print(F("[CFG] Loaded "));
    Serial.println(buildSettingsPayload("CFG"));
    Serial.print(F("[WIFI] Loaded "));
    Serial.println(buildWifiPayload("WIFI"));
    Serial.print(F("[WEB] Loaded "));
    Serial.println(webPublishStatusPayload("WEB"));

    if (wifiAutoConnect && wifiSavedSsid.length() > 0) {
      startWifiConnect();
      setLedBaseState(LED_BASE_WIFI_CONNECTING);
    }
  } else {
    Serial.println(F("[CFG] Preferences init failed, using defaults"));
    pulseLedWarn();
  }

  byte version = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println(F("Khong tim thay RC522!"));
    setLedBaseState(LED_BASE_WIFI_ERROR);
    pulseLedError();
    while (1) {
      updateLedStatus();
      delay(8);
    }
  }

  webPublishBegin(webServerUrl, webGameID, webEnabled);

  // Board ID = full MAC address (lowercase, dash-separated): "dc-b4-d9-14-56-5c"
  // Auto-migrate old short format (board_XXXX, no dash) to full MAC on first boot.
  {
    bool needsNewId = boardID.length() == 0 || boardID.indexOf('-') < 0;
    if (needsNewId) {
      uint8_t mac[6];
      esp_read_mac(mac, ESP_MAC_WIFI_STA);
      char buf[18];
      snprintf(buf, sizeof(buf), "%02x-%02x-%02x-%02x-%02x-%02x",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      boardID = String(buf);
      if (settingsStoreReady) {
        settingsStore.putString(KEY_BOARD_ID, boardID);
      }
      Serial.print(F("[REG] boardID="));
      Serial.println(boardID);
    }
  }

  boardRegBegin(webServerUrl, boardID, webGameID);
  boardRegSetGameIDCallback(onBoardRegGameIDAssigned);
  boardRegSetCommandCallback(onBoardRegCommandReceived);
  boardRegSetFastPoll(true);  // fast until game starts (updated in tick)

  prepareNewGameModel();
  gameStarted = false;

  Serial.println(F("[BOOT] He thong san sang."));
  Serial.println(F("[BOOT] Dat quan dung vi tri ban dau, gui START de bat dau."));
  printHelp();
  setLedBaseState(LED_BASE_IDLE_DISCONNECTED);
  updateLedStatus();
}

void smartChessTick() {
  bleServicePoll();
  processSerialCommands();

  wl_status_t wifiStatus = WiFi.status();
  bool wifiConnectedNow = (wifiStatus == WL_CONNECTED);

  if (wifiConnectInProgress) {
    if (wifiConnectedNow) {
      wifiConnectInProgress = false;
      wifiConnectFailed = false;
      wifiLastError = "";
      wifiRetryScheduledMs = 0;
      bleLog(String("[WIFI] CONNECTED ip=") + WiFi.localIP().toString());
      pulseLedOk();
    } else if (wifiStatus == WL_CONNECT_FAILED) {
      wifiConnectInProgress = false;
      wifiConnectFailed = true;
      wifiLastError = "AUTH";
      bleLog(String("[WIFI] CONNECT_FAILED auth ssid=") + wifiSavedSsid);
      pulseLedError();
      if (wifiAutoConnect) wifiRetryScheduledMs = millis() + WIFI_RETRY_DELAY_MS;
    } else if (wifiStatus == WL_NO_SSID_AVAIL) {
      wifiConnectInProgress = false;
      wifiConnectFailed = true;
      wifiLastError = "NO_SSID";
      bleLog(String("[WIFI] CONNECT_FAILED no_ssid ssid=") + wifiSavedSsid);
      pulseLedError();
      if (wifiAutoConnect) wifiRetryScheduledMs = millis() + WIFI_RETRY_DELAY_MS;
    } else if (millis() - wifiConnectStartMs > WIFI_CONNECT_TIMEOUT_MS) {
      wifiConnectInProgress = false;
      wifiConnectFailed = true;
      wifiLastError = "TIMEOUT";
      bleLog(String("[WIFI] CONNECT_TIMEOUT ssid=") + wifiSavedSsid);
      pulseLedError();
      if (wifiAutoConnect) wifiRetryScheduledMs = millis() + WIFI_RETRY_DELAY_MS;
    }
  }

  // Auto-retry after a failed connection attempt (covers AUTH/NO_SSID/TIMEOUT)
  if (wifiRetryScheduledMs > 0 && millis() >= wifiRetryScheduledMs) {
    wifiRetryScheduledMs = 0;
    if (!wifiConnectedNow && wifiAutoConnect && wifiSavedSsid.length() > 0) {
      bleLog(String("[WIFI] AUTO_RETRY ssid=") + wifiSavedSsid);
      startWifiConnect();
    }
  }

  if (wifiConnectedNow != wifiLastConnected) {
    wifiLastConnected = wifiConnectedNow;
    if (!wifiConnectedNow && wifiAutoConnect && wifiSavedSsid.length() > 0) {
      // WiFi dropped while connected — reconnect immediately
      wifiRetryScheduledMs = 0;
      startWifiConnect();
      pulseLedWarn();
    }
  }

  refreshLedBaseState();
  updateLedStatus();

  processLichessUploadTick();
  esp_task_wdt_delete(NULL);
  webPublishPoll();
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();

  // After each poll, check whether the server rejected our last move.
  // A 4xx response means chess.js considered the move illegal (e.g. it left the
  // king in check).  We must:
  //   1. Roll back totalMovesCount (seq desync prevention)
  //   2. Restore the local board model to its pre-move state (whiteTurn, board
  //      squares, UIDs, clocks) so subsequent moves are validated correctly.
  if (webMovePendingConfirm) {
    int httpStatus = webPublishGetLastStatus();
    if (httpStatus >= 400 && httpStatus < 500) {
      // ── seq rollback ────────────────────────────────────────────────────────
      if (totalMovesCount > 0) totalMovesCount--;
      webMovePendingConfirm = false;

      // ── board model undo ────────────────────────────────────────────────────
      // Restore key state saved just before applyMove() was called.
      // This covers the most common SERVER_ILLEGAL scenario (piece pinned / move
      // exposes king to check).  Edge cases: en-passant capture undo and
      // un-marking a captured piece in pieceDB are skipped for simplicity.
      if (savedRollbackFrom >= 0) {
        int ff = savedRollbackFrom / 8, fr = savedRollbackFrom % 8;
        int tf = savedRollbackTo   / 8, tr = savedRollbackTo   % 8;
        board[ff][fr]               = savedRollbackPiece;
        board[tf][tr]               = savedRollbackTarget;
        squareUID[savedRollbackFrom] = savedRollbackUID;
        squareUID[savedRollbackTo]   = savedRollbackTgtUID;
        whiteTurn      = savedRollbackWTurn;
        halfmoveClock  = savedRollbackHalf;
        fullmoveNumber = savedRollbackFull;
        enPassantFile  = savedRollbackEPFile;
        enPassantRank  = savedRollbackEPRank;
        rebuildOccupiedListFromBoard();
        savedRollbackFrom = -1;
        Serial.println(F("[ROLLBACK] Board model restored after server rejection"));
      }

      pulseLedError();
      bleLog(F("[ERR] SERVER_ILLEGAL | illegal move rejected by server — place piece back"));
      boardRegQueueAlert("SERVER_ILLEGAL", "move rejected");
    } else if (httpStatus >= 200 && httpStatus < 300) {
      // Server accepted — clear snapshot
      savedRollbackFrom = -1;
      webMovePendingConfirm = false;
    }
    // httpStatus == 0: POST not yet completed — check again next loop iteration
  }

  // Sync fast/normal heartbeat interval with game state
  boardRegSetFastPoll(!gameStarted);

  // Execute WiFi-triggered START (set by onBoardRegCommandReceived)
  if (pendingWifiStart) {
    pendingWifiStart = false;
    pulseLedInfo();
    if (startAndLearnInitial32()) {
      pulseLedOk();
      boardRegQueueScanResult("STARTED", "32");
      bleLog(F("[OK] WiFi START — game started, 32 pieces confirmed"));
    } else {
      pulseLedError();
      boardRegQueueScanResult(startFailReason, startFailDetail);
      bleLog(String(F("[ERR] WiFi START failed: ")) + startFailReason
             + (startFailDetail.length() ? String(F(" | ")) + startFailDetail : String()));
    }
  }

  // HTTPS calls (WiFiClientSecure TLS handshake) can block >10 s on Render
  // cold-starts — temporarily remove this task from the watchdog so it
  // doesn't fire during the network call.
  esp_task_wdt_delete(NULL);
  boardRegTick();
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();

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

  if (scanVerbose && scanState != SCAN_IDLE) {
    Serial.print(F("[STATE] "));
    Serial.println((int)scanState);
  }

  esp_task_wdt_reset();
  delay(1);
}


