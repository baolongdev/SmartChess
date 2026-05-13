#include "app/SmartChessApp.h"

#include <Arduino.h>
#include <esp32-hal-rgb-led.h>
#include <esp_task_wdt.h>
#include <MFRC522.h>
#include <Preferences.h>
#include <WiFi.h>

#include "chess/BoardState.h"
#include "chess/FenBuilder.h"
#include "ble/BleService.h"
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

// ---------------------------------------------------------------------------
// Button — GPIO 0 (BOOT button on most ESP32 dev boards, internal pull-up)
// ---------------------------------------------------------------------------
constexpr int BUTTON_PIN = 0;
static volatile bool buttonInterruptFlag = false;
static unsigned long lastButtonMs = 0;

void IRAM_ATTR onButtonPress() {
  buttonInterruptFlag = true;
}

// ---------------------------------------------------------------------------
// RFID + Board model
// ---------------------------------------------------------------------------
static MFRC522 mfrc522(SS_PIN, RST_PIN);

static char board[8][8];
static String squareUID[NUM_ANTENNAS];
static String scanBuf[NUM_ANTENNAS];
static PieceInfo pieceDB[MAX_PIECES];
static int pieceDBCount = 0;

static bool whiteTurn = true;
static int moveSeq = 0;

// ---------------------------------------------------------------------------
// Scan settings — persisted in NVS
// ---------------------------------------------------------------------------
static bool scanVerbose = false;
static bool scanContinuous = true;
static bool scanUsePieceLetters = true;
static unsigned long lastScanPrintMs = 0;

// ---------------------------------------------------------------------------
// WiFi state
// ---------------------------------------------------------------------------
static String wifiSavedSsid = "";
static String wifiSavedPass = "";
static bool wifiAutoConnect = true;
static bool wifiConnectInProgress = false;
static unsigned long wifiConnectStartMs = 0;
static bool wifiLastConnected = false;
static unsigned long wifiRetryScheduledMs = 0;
static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr unsigned long WIFI_RETRY_DELAY_MS = 30000;
static constexpr int WIFI_SCAN_MAX = 24;
static String wifiScanResults[WIFI_SCAN_MAX];
static int wifiScanRssi[WIFI_SCAN_MAX];
static int wifiScanCount = 0;
static String wifiScanLastError = "";
static bool wifiConnectFailed = false;
static String wifiLastError = "";

// ---------------------------------------------------------------------------
// OTA over BLE
// ---------------------------------------------------------------------------
static esp_ota_handle_t            gOtaHandle = 0;
static const esp_partition_t      *gOtaPartition = nullptr;
static bool                        gOtaInProgress = false;
static size_t                      gOtaBytesWritten = 0;

// ---------------------------------------------------------------------------
// Web publish
// ---------------------------------------------------------------------------
static String webServerUrl = "";
static String webGameID = "";
static String boardID = "";
static bool webEnabled = false;

// ---------------------------------------------------------------------------
// NVS Preferences
// ---------------------------------------------------------------------------
static Preferences settingsStore;
static bool settingsStoreReady = false;
static constexpr const char *SETTINGS_NS = "smartchess";
static constexpr const char *KEY_CFG_VER = "cfg_ver";
static constexpr const char *KEY_SCAN_V = "scan_v";
static constexpr const char *KEY_SCAN_C = "scan_c";
static constexpr const char *KEY_SCAN_L = "scan_l";
static constexpr const char *KEY_WIFI_VER = "wifi_ver";
static constexpr const char *KEY_WIFI_SSID = "wifi_ssid";
static constexpr const char *KEY_WIFI_PASS = "wifi_pass";
static constexpr const char *KEY_WIFI_AUTO = "wifi_auto";
static constexpr const char *KEY_WEB_URL = "web_url";
static constexpr const char *KEY_WEB_GAME = "web_game";
static constexpr const char *KEY_WEB_ENABLED = "web_enabled";
static constexpr const char *KEY_BOARD_ID = "board_id";

static uint32_t settingsVersion = 1;
static uint32_t wifiSettingsVersion = 1;

// ---------------------------------------------------------------------------
// LED control
// ---------------------------------------------------------------------------
enum LedState {
  LED_BOOT,
  LED_IDLE,
  LED_SCANNING,
  LED_PUBLISH_OK,
  LED_PUBLISH_ERR,
  LED_WIFI_CONNECTING,
  LED_WIFI_ERROR,
};

static LedState ledState = LED_BOOT;
static bool ledInitialized = false;
static unsigned long ledPulseUntilMs = 0;
static uint8_t ledPulseR = 0;
static uint8_t ledPulseG = 0;
static uint8_t ledPulseB = 0;

static void setRgbNow(uint8_t r, uint8_t g, uint8_t b) {
#if defined(RGB_BUILTIN)
  neopixelWrite(RGB_BUILTIN, r, g, b);
#else
  (void)r; (void)g; (void)b;
#endif
}

static void pulseLed(uint8_t r, uint8_t g, uint8_t b, unsigned long holdMs) {
  ledPulseR = r; ledPulseG = g; ledPulseB = b;
  ledPulseUntilMs = millis() + holdMs;
}

static void pulseLedOk()    { pulseLed(0, 42, 0, 260); }
static void pulseLedWarn()  { pulseLed(38, 14, 0, 360); }
static void pulseLedError() { pulseLed(40, 0, 0, 520); }
static void pulseLedInfo()  { pulseLed(0, 18, 28, 220); }

static void setLedBaseState(LedState st) { ledState = st; }

static void refreshLedBaseState() {
  if (wifiConnectInProgress) {
    setLedBaseState(LED_WIFI_CONNECTING);
    return;
  }
  if (wifiConnectFailed) {
    setLedBaseState(LED_WIFI_ERROR);
    return;
  }
  setLedBaseState(LED_IDLE);
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

  switch (ledState) {
    case LED_BOOT: {
      bool on = ((now / 180) % 2) == 0;
      setRgbNow(0, 0, on ? 20 : 0);
      break;
    }
    case LED_IDLE:
      setRgbNow(0, 6, 0);
      break;
    case LED_SCANNING: {
      bool on = ((now / 120) % 2) == 0;
      setRgbNow(on ? 24 : 8, on ? 10 : 3, 0);
      break;
    }
    case LED_WIFI_CONNECTING: {
      bool on = ((now / 260) % 2) == 0;
      setRgbNow(on ? 24 : 3, on ? 10 : 1, 0);
      break;
    }
    case LED_WIFI_ERROR: {
      bool on = ((now / 180) % 2) == 0;
      setRgbNow(on ? 24 : 0, 0, 0);
      break;
    }
    case LED_PUBLISH_OK:
    case LED_PUBLISH_ERR:
      // Pulse takes priority; handled above via ledPulseUntilMs
      break;
  }
}

// ---------------------------------------------------------------------------
// OTA handler
// ---------------------------------------------------------------------------
static void onBleOtaData(const uint8_t *data, size_t len) {
  if (len == 0) return;

  if (len >= 9 && memcmp(data, "OTA_BEGIN", 9) == 0) {
    gOtaPartition = esp_ota_get_next_update_partition(nullptr);
    if (!gOtaPartition) {
      bleOtaRespond("OTA_ERR:NO_PARTITION");
      return;
    }
    esp_err_t err = esp_ota_begin(gOtaPartition, OTA_SIZE_UNKNOWN, &gOtaHandle);
    if (err != ESP_OK) {
      bleOtaRespond("OTA_ERR:BEGIN_FAILED");
      return;
    }
    gOtaInProgress = true;
    gOtaBytesWritten = 0;
    bleOtaRespond("OTA_READY");
    return;
  }

  if (len == 7 && memcmp(data, "OTA_END", 7) == 0) {
    if (!gOtaInProgress) { bleOtaRespond("OTA_ERR:NOT_STARTED"); return; }
    gOtaInProgress = false;
    esp_err_t err = esp_ota_end(gOtaHandle);
    if (err != ESP_OK) { bleOtaRespond("OTA_ERR:END_FAILED"); return; }
    err = esp_ota_set_boot_partition(gOtaPartition);
    if (err != ESP_OK) { bleOtaRespond("OTA_ERR:SET_BOOT_FAILED"); return; }
    bleOtaRespond("OTA_OK");
    delay(500);
    esp_restart();
    return;
  }

  if (gOtaInProgress) {
    esp_err_t err = esp_ota_write(gOtaHandle, data, len);
    if (err != ESP_OK) {
      gOtaInProgress = false;
      bleOtaRespond("OTA_ERR:WRITE_FAILED");
      return;
    }
    gOtaBytesWritten += len;
    char ack[40];
    snprintf(ack, sizeof(ack), "OTA_ACK:%zu", gOtaBytesWritten);
    bleOtaRespond(ack);
  }
}

// ---------------------------------------------------------------------------
// WiFi helpers
// ---------------------------------------------------------------------------
static bool startWifiConnect() {
  if (wifiSavedSsid.length() == 0) return false;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
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
  wifiRetryScheduledMs = 0;
  pulseLedInfo();
}

static int performWifiScan() {
  bool resumeConnectAfterScan = wifiConnectInProgress;
  if (resumeConnectAfterScan) wifiConnectInProgress = false;

  WiFi.mode(WIFI_STA);
  delay(40);
  WiFi.scanDelete();

  int n = -1;
  for (int attempt = 0; attempt < 3; attempt++) {
    n = WiFi.scanNetworks(false, true);
    if (n >= 0) break;
    delay(120 + attempt * 80);
  }

  if (n < 0) { wifiScanLastError = "SCAN_FAIL"; n = 0; }
  else if (n == 0) { wifiScanLastError = "NO_AP"; }
  else { wifiScanLastError = ""; }

  if (n > WIFI_SCAN_MAX) n = WIFI_SCAN_MAX;
  wifiScanCount = n;
  for (int i = 0; i < wifiScanCount; i++) {
    String ssid = WiFi.SSID(i);
    ssid.replace("|", " ");
    wifiScanResults[i] = ssid;
    wifiScanRssi[i] = (int)WiFi.RSSI(i);
  }
  for (int i = wifiScanCount; i < WIFI_SCAN_MAX; i++) {
    wifiScanResults[i] = "";
    wifiScanRssi[i] = 0;
  }
  WiFi.scanDelete();

  if (resumeConnectAfterScan && WiFi.status() != WL_CONNECTED) {
    wifiConnectInProgress = true;
  }
  return wifiScanCount;
}

static String wifiStatusText() {
  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) return "CONNECTED";
  if (wifiConnectFailed)  return "FAILED";
  if (wifiConnectInProgress) return "CONNECTING";
  return "DISCONNECTED";
}

static String wifiIpText() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return "0.0.0.0";
}

static char wifiPayloadBuf[192];
static const char* buildWifiPayload(const char* prefix) {
  String mac = WiFi.macAddress();
  snprintf(wifiPayloadBuf, sizeof(wifiPayloadBuf),
    "%s|ver=%lu|ssid=%s|auto=%d|status=%s|ip=%s|mac=%s|err=%s",
    prefix, wifiSettingsVersion,
    wifiSavedSsid.c_str(), wifiAutoConnect ? 1 : 0,
    wifiStatusText().c_str(), wifiIpText().c_str(),
    mac.c_str(), wifiLastError.c_str());
  return wifiPayloadBuf;
}

static String buildWifiScanPayload(const String &prefix) {
  String payload = prefix;
  payload += "|count="; payload += String(wifiScanCount);
  payload += "|err=";   payload += wifiScanLastError;
  return payload;
}

static String buildWifiScanItemPayload(int idx, const String &prefix) {
  String payload = prefix;
  payload += "|idx=";  payload += String(idx);
  payload += "|ssid=";
  if (idx >= 0 && idx < wifiScanCount) {
    payload += wifiScanResults[idx];
    payload += "|rssi="; payload += String(wifiScanRssi[idx]);
  }
  return payload;
}

// ---------------------------------------------------------------------------
// NVS persistence
// ---------------------------------------------------------------------------
static void persistWifiSettingsToStore() {
  if (!settingsStoreReady) return;
  settingsStore.putULong(KEY_WIFI_VER, wifiSettingsVersion);
  settingsStore.putString(KEY_WIFI_SSID, wifiSavedSsid);
  settingsStore.putString(KEY_WIFI_PASS, wifiSavedPass);
  settingsStore.putBool(KEY_WIFI_AUTO, wifiAutoConnect);
}

static void loadWifiSettingsFromStore() {
  if (!settingsStoreReady) return;
  wifiSettingsVersion = settingsStore.getULong(KEY_WIFI_VER, 1UL);
  if (wifiSettingsVersion == 0) wifiSettingsVersion = 1;
  wifiSavedSsid = settingsStore.getString(KEY_WIFI_SSID, "");
  wifiSavedPass = settingsStore.getString(KEY_WIFI_PASS, "");
  wifiAutoConnect = settingsStore.getBool(KEY_WIFI_AUTO, true);
}

static void bumpWifiSettingsVersion() {
  wifiSettingsVersion = (wifiSettingsVersion >= 0xFFFFFFFFUL) ? 1 : wifiSettingsVersion + 1;
}

static char settingsPayloadBuf[64];
static const char* buildSettingsPayload(const char* prefix) {
  snprintf(settingsPayloadBuf, sizeof(settingsPayloadBuf),
    "%s|ver=%lu|verbose=%d|continuous=%d|letters=%d",
    prefix, settingsVersion,
    scanVerbose ? 1 : 0, scanContinuous ? 1 : 0, scanUsePieceLetters ? 1 : 0);
  return settingsPayloadBuf;
}

static void bumpSettingsVersion() {
  settingsVersion = (settingsVersion >= 0xFFFFFFFFUL) ? 1 : settingsVersion + 1;
}

static void persistSettingsToStore() {
  if (!settingsStoreReady) return;
  settingsStore.putULong(KEY_CFG_VER, settingsVersion);
  settingsStore.putBool(KEY_SCAN_V, scanVerbose);
  settingsStore.putBool(KEY_SCAN_C, scanContinuous);
  settingsStore.putBool(KEY_SCAN_L, scanUsePieceLetters);
}

static void loadSettingsFromStore() {
  if (!settingsStoreReady) return;
  settingsVersion = settingsStore.getULong(KEY_CFG_VER, 1UL);
  if (settingsVersion == 0) settingsVersion = 1;
  scanVerbose = settingsStore.getBool(KEY_SCAN_V, false);
  scanContinuous = settingsStore.getBool(KEY_SCAN_C, true);
  scanUsePieceLetters = settingsStore.getBool(KEY_SCAN_L, true);
}

static void saveSettingsAfterLocalChange() {
  bumpSettingsVersion();
  persistSettingsToStore();
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

// ---------------------------------------------------------------------------
// Board-UUID helpers
// ---------------------------------------------------------------------------
static char lookupUID(const String &uid) {
  for (int i = 0; i < pieceDBCount; i++) {
    if (pieceDB[i].uid == uid) return pieceDB[i].piece;
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

// ---------------------------------------------------------------------------
// FEN builder wrapper
// ---------------------------------------------------------------------------
static char fenBuffer[128];
static const char* buildCurrentFen() {
  int fullmoveNumber = (moveSeq / 2) + 1;
  fenBuild(board, fenBuffer, sizeof(fenBuffer),
           whiteTurn,
           -1, -1,   // en passant = none
           0,         // halfmove clock
           fullmoveNumber,
           false, false, false, false, false, false);
  return fenBuffer;
}

// ---------------------------------------------------------------------------
// scanAllAndPublish — core new function
// ---------------------------------------------------------------------------
static void scanAllAndPublish() {
  pulseLedInfo();
  unsigned long scanStart = millis();

  bleLog(F("[SCAN] Scanning all 64 squares..."));

  // 1. Burst scan all squares
  for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
    String uid = scanUID(mfrc522, idx);
    int file = idx / 8;
    int rank = idx % 8;

    if (uid.length() == 0) {
      board[file][rank] = '.';
      squareUID[idx] = "";
    } else {
      char piece = lookupUID(uid);
      if (piece == '?') {
        board[file][rank] = '?';
      } else {
        board[file][rank] = piece;
      }
      squareUID[idx] = uid;
    }

    if (idx % 8 == 0) {
      esp_task_wdt_reset();
    }
  }

  // 2. Toggle turn
  whiteTurn = !whiteTurn;

  // 3. Build FEN
  const char *fen = buildCurrentFen();

  unsigned long elapsed = millis() - scanStart;
  bleLog(String("[SCAN] Done ") + String(elapsed) + "ms | " + String(fen));
  Serial.print(F("[SCAN] "));
  Serial.println(fen);
  printBoard(board);

  // 4. Publish
  moveSeq++;
  webPublishFEN(String(fen), moveSeq);
  bleUpdateFEN(String(fen));

  pulseLedOk();
}

// ---------------------------------------------------------------------------
// CFG_SET command handler
// ---------------------------------------------------------------------------
static bool handleCfgSetCommand(const String &cmd, String &response) {
  bool hasAnyField = false;
  bool boolValue = false;

  String vVerbose = settingsFieldValue(cmd, "VERBOSE");
  if (vVerbose.length() > 0) {
    if (!parseBoolToken(vVerbose, boolValue)) {
      response = "CFG_ERR:BAD_VERBOSE"; return false;
    }
    scanVerbose = boolValue; hasAnyField = true;
  }

  String vContinuous = settingsFieldValue(cmd, "CONTINUOUS");
  if (vContinuous.length() > 0) {
    if (!parseBoolToken(vContinuous, boolValue)) {
      response = "CFG_ERR:BAD_CONTINUOUS"; return false;
    }
    scanContinuous = boolValue; hasAnyField = true;
  }

  String vLetters = settingsFieldValue(cmd, "LETTERS");
  if (vLetters.length() > 0) {
    if (!parseBoolToken(vLetters, boolValue)) {
      response = "CFG_ERR:BAD_LETTERS"; return false;
    }
    scanUsePieceLetters = boolValue; hasAnyField = true;
  }

  if (!hasAnyField) { response = "CFG_ERR:NO_FIELDS"; return false; }

  uint32_t incomingVer = (uint32_t)strtoul(settingsFieldValue(cmd, "VER").c_str(), nullptr, 10);
  if (incomingVer > settingsVersion) settingsVersion = incomingVer;
  else bumpSettingsVersion();

  persistSettingsToStore();
  response = buildSettingsPayload("CFG_SAVED");
  bleLog(String("[CFG] SAVED ") + response);
  return true;
}

// ---------------------------------------------------------------------------
// WiFi SET command handler
// ---------------------------------------------------------------------------
static bool handleWifiSetCommand(const String &cmd, String &response) {
  bool hasAnyField = false;
  bool boolValue = false;

  String vSsid = settingsFieldValue(cmd, "SSID");
  String vPass = settingsFieldValue(cmd, "PASS");
  String vAuto = settingsFieldValue(cmd, "AUTO");
  String vConnect = settingsFieldValue(cmd, "CONNECT");

  String cmdUpper = cmd; cmdUpper.toUpperCase();

  if (vSsid.length() > 0 || cmdUpper.indexOf("SSID=") >= 0) { wifiSavedSsid = vSsid; hasAnyField = true; }
  if (vPass.length() > 0 || cmdUpper.indexOf("PASS=") >= 0) { wifiSavedPass = vPass; hasAnyField = true; }

  if (vAuto.length() > 0) {
    if (!parseBoolToken(vAuto, boolValue)) { response = "WIFI_ERR:BAD_AUTO"; return false; }
    wifiAutoConnect = boolValue; hasAnyField = true;
  }

  if (!hasAnyField && vConnect.length() == 0) { response = "WIFI_ERR:NO_FIELDS"; return false; }

  uint32_t incomingVer = (uint32_t)strtoul(settingsFieldValue(cmd, "VER").c_str(), nullptr, 10);
  if (incomingVer > wifiSettingsVersion) wifiSettingsVersion = incomingVer;
  else if (hasAnyField) bumpWifiSettingsVersion();

  if (hasAnyField) persistWifiSettingsToStore();

  bool forceConnect = false;
  if (vConnect.length() > 0) {
    if (!parseBoolToken(vConnect, boolValue)) { response = "WIFI_ERR:BAD_CONNECT"; return false; }
    forceConnect = boolValue;
  }

  if (forceConnect || wifiAutoConnect) {
    if (wifiSavedSsid.length() == 0) { response = "WIFI_ERR:EMPTY_SSID"; return false; }
    startWifiConnect();
  }

  response = buildWifiPayload("WIFI_SAVED");
  bleLog(String("[WIFI] SAVED ") + response);
  return true;
}

// ---------------------------------------------------------------------------
// WEB_SET command handler
// ---------------------------------------------------------------------------
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
    if (!parseBoolToken(vEnabled, b)) { response = "WEB_ERR:BAD_ENABLED"; return false; }
    webEnabled = b; hasAnyField = true;
  }

  if (!hasAnyField) { response = "WEB_ERR:NO_FIELDS"; return false; }

  webPublishSetConfig(webServerUrl, webGameID, boardID, webEnabled);
  boardRegSetConfig(webServerUrl, boardID);
  persistWebSettingsToStore();
  boardRegForceHeartbeat();

  response = webPublishStatusPayload("WEB_SAVED");
  return true;
}

// ---------------------------------------------------------------------------
// BoardRegistration callbacks
// ---------------------------------------------------------------------------
static void onBoardRegGameIDAssigned(const String &newGameID) {
  webGameID = newGameID;
  webPublishSetConfig(webServerUrl, webGameID, boardID, webEnabled);
  persistWebSettingsToStore();
  bleLog(String("[REG] gameID assigned=") + newGameID);
}

// ---------------------------------------------------------------------------
// Common BLE commands shared with serial path
// ---------------------------------------------------------------------------
static bool handleCommonCommand(const String &cmd, const String &cmdUpper,
                                String &response, bool &handled) {

  if (cmdUpper == "CFG" || cmdUpper == "CFG?" || cmdUpper == "CFG_GET") {
    handled = true; response = buildSettingsPayload("CFG"); return true;
  }
  if (cmdUpper.startsWith("CFG_SET")) {
    handled = true; return handleCfgSetCommand(cmd, response);
  }

  if (cmdUpper == "WIFI" || cmdUpper == "WIFI?" || cmdUpper == "WIFI_GET") {
    handled = true; response = buildWifiPayload("WIFI"); return true;
  }
  if (cmdUpper == "WIFI_SCAN") {
    handled = true; performWifiScan();
    for (int i = 0; i < wifiScanCount; i++) {
      String net = String("[WIFI_NET] idx=") + i + "|ssid=" + wifiScanResults[i] + "|rssi=" + String(wifiScanRssi[i]);
      bleLogImmediate(net);
    }
    response = buildWifiScanPayload("WIFI_SCAN"); return true;
  }
  if (cmdUpper.startsWith("WIFI_SCAN_ITEM")) {
    handled = true;
    String idxToken = settingsFieldValue(cmd, "IDX");
    if (idxToken.length() == 0) { response = "WIFI_ERR:BAD_IDX"; return false; }
    int idx = idxToken.toInt();
    if (idx < 0 || idx >= wifiScanCount) { response = "WIFI_ERR:IDX_RANGE"; return false; }
    response = buildWifiScanItemPayload(idx, "WIFI_SCAN_ITEM"); return true;
  }
  if (cmdUpper.startsWith("WIFI_SET")) {
    handled = true; return handleWifiSetCommand(cmd, response);
  }
  if (cmdUpper == "WIFI_CONNECT") {
    handled = true;
    if (startWifiConnect()) { response = buildWifiPayload("WIFI_CONNECTING"); return true; }
    response = "WIFI_ERR:EMPTY_SSID"; return false;
  }
  if (cmdUpper == "WIFI_DISCONNECT") {
    handled = true; disconnectWifiNow();
    response = buildWifiPayload("WIFI_DISCONNECTED"); return true;
  }

  if (cmdUpper == "WEB" || cmdUpper == "WEB?" || cmdUpper == "WEB_GET") {
    handled = true; response = webPublishStatusPayload("WEB"); return true;
  }
  if (cmdUpper.startsWith("WEB_SET")) {
    handled = true; return handleWebSetCommand(cmd, response);
  }

  handled = false; return false;
}

// ---------------------------------------------------------------------------
// BLE command handler
// ---------------------------------------------------------------------------
static bool handleBleCommand(const String &raw, String &response) {
  String cmd = raw; cmd.trim();
  String cmdUpper = cmd; cmdUpper.toUpperCase();

  if (cmdUpper.length() == 0) { response = "EMPTY"; return false; }

  // SNAPSHOT — manual trigger (same as pressing the button)
  if (cmdUpper == "SNAPSHOT") {
    scanAllAndPublish();
    response = "SNAPSHOT_OK";
    return true;
  }

  if (cmdUpper == "STATUS") {
    String s = "IDLE";
    s += "|turn=";   s += (whiteTurn ? "WHITE" : "BLACK");
    s += "|seq=";    s += moveSeq;
    s += "|ble=";    s += (bleIsConnected() ? 1 : 0);
    s += "|wifi=";   s += wifiStatusText();
    if (WiFi.status() == WL_CONNECTED) { s += "|ip="; s += WiFi.localIP().toString(); }
    s += "|web=";    s += (webEnabled ? 1 : 0);
    s += "|board=";  s += (boardID.length() > 0 ? boardID : "-");
    s += "|pieces="; s += pieceDBCount;
    response = s;
    pulseLedInfo();
    return true;
  }

  if (cmdUpper == "F" || cmdUpper == "FEN") {
    const char *fen = buildCurrentFen();
    response = String(fen);
    pulseLedInfo();
    return true;
  }

  if (cmdUpper == "CLEAR") {
    Serial.print(F("\033[2J\033[H"));
    response = "OK"; return true;
  }

  if (cmdUpper == "LEARN") {
    // LEARN|a1=P a2=P ...  — used to associate UIDs with piece types
    // Scan all squares, detect UIDs, register each with its standard piece
    initStandardBoard(board);
    pieceDBCount = 0;
    int learned = 0;
    for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
      String uid = scanUID(mfrc522, idx);
      if (uid.length() > 0) {
        int file = idx / 8;
        int rank = idx % 8;
        char piece = board[file][rank];
        if (piece != '.') {
          registerPiece(uid, piece);
          learned++;
        }
      }
    }
    // Set turn back to white, reset seq
    whiteTurn = true;
    moveSeq = 0;
    String msg = String("LEARNED pieces=") + learned + "/32";
    bleLog(msg);
    response = msg;
    pulseLedOk();
    return true;
  }

  bool handled = false;
  if (handleCommonCommand(cmd, cmdUpper, response, handled)) return true;
  if (handled) return false;

  response = "UNKNOWN_CMD";
  bleLog(String("[ERR] UNKNOWN_CMD ") + cmdUpper);
  pulseLedError();
  return false;
}

// ---------------------------------------------------------------------------
// Serial command handler
// ---------------------------------------------------------------------------
static String cmdBuffer = "";
static unsigned long cmdLastByteMs = 0;
static constexpr unsigned long CMD_COMMIT_IDLE_MS = 120;

static void printHelp() {
  Serial.println(F("=================================================="));
  Serial.println(F("  SmartChess — COMMANDS"));
  Serial.println(F("=================================================="));
  Serial.println(F("  SNAPSHOT     scan all squares + publish FEN"));
  Serial.println(F("  STATUS       board + WiFi + BLE status"));
  Serial.println(F("  F / FEN      print current FEN"));
  Serial.println(F("  LEARN        scan + learn 32 piece UIDs"));
  Serial.println(F("  CFG          read settings"));
  Serial.println(F("  CFG_SET|...  write settings"));
  Serial.println(F("  WIFI         WiFi status"));
  Serial.println(F("  WIFI_SET|... configure WiFi"));
  Serial.println(F("  WIFI_SCAN    scan networks"));
  Serial.println(F("  WIFI_CONNECT/WIFI_DISCONNECT"));
  Serial.println(F("  WEB          web publish config"));
  Serial.println(F("  WEB_SET|...  configure web publish"));
  Serial.println(F("  CLEAR        clear screen"));
  Serial.println(F("  HELP         this help"));
  Serial.println(F("=================================================="));
}

static void handleCommand(const String &raw) {
  String cmd = raw; cmd.trim();
  String cmdUpper = cmd; cmdUpper.toUpperCase();

  if (cmdUpper.length() == 0) return;

  if (cmdUpper == "HELP") { printHelp(); return; }
  if (cmdUpper == "CLEAR") {
    Serial.print(F("\033[2J\033[H"));
    Serial.println(F("[SmartChess] Screen cleared."));
    return;
  }
  if (cmdUpper == "SNAPSHOT") {
    scanAllAndPublish(); return;
  }
  if (cmdUpper == "STATUS") {
    pulseLedInfo();
    Serial.print(F("[STATE] turn="));
    Serial.print(whiteTurn ? "WHITE" : "BLACK");
    Serial.print(F(" seq="));
    Serial.print(moveSeq);
    Serial.print(F(" pieces="));
    Serial.println(pieceDBCount);
    return;
  }
  if (cmdUpper == "F" || cmdUpper == "FEN") {
    Serial.print(F("[FEN] "));
    Serial.println(buildCurrentFen());
    return;
  }
  if (cmdUpper == "LEARN") {
    initStandardBoard(board);
    pieceDBCount = 0;
    int learned = 0;
    for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
      String uid = scanUID(mfrc522, idx);
      if (uid.length() > 0) {
        int file = idx / 8;
        int rank = idx % 8;
        char piece = board[file][rank];
        if (piece != '.') { registerPiece(uid, piece); learned++; }
      }
    }
    whiteTurn = true;
    moveSeq = 0;
    Serial.print(F("[LEARN] Learned "));
    Serial.print(learned);
    Serial.println(F("/32 pieces"));
    return;
  }

  String response; bool handled = false;
  handleCommonCommand(cmd, cmdUpper, response, handled);
  if (handled) {
    if (response.startsWith("WIFI_ERR") || response.startsWith("CFG_ERR")) {
      pulseLedError();
    } else { pulseLedInfo(); }
    if (response.length() > 0) {
      bool isErr = response.startsWith("WIFI_ERR") || response.startsWith("CFG_ERR");
      if (isErr) Serial.print(F("[ERR] ")); else Serial.print(F("[OK] "));
      Serial.println(response);
    }
    return;
  }

  Serial.print(F("[CMD] Unknown: ")); Serial.println(cmd);
  pulseLedError();
}

static void processSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (cmdBuffer.length() > 0) {
        handleCommand(cmdBuffer);
        cmdBuffer = ""; cmdLastByteMs = 0;
      }
      continue;
    }
    if (cmdBuffer.length() < 256) { cmdBuffer += c; cmdLastByteMs = millis(); }
  }
  if (cmdBuffer.length() > 0 && cmdLastByteMs > 0 &&
      millis() - cmdLastByteMs >= CMD_COMMIT_IDLE_MS) {
    handleCommand(cmdBuffer);
    cmdBuffer = ""; cmdLastByteMs = 0;
  }
}

// ---------------------------------------------------------------------------
// smartChessBegin
// ---------------------------------------------------------------------------
void smartChessBegin() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 3000) {}

#if defined(RGB_BUILTIN)
  pinMode(RGB_BUILTIN, OUTPUT);
#endif
  setLedBaseState(LED_BOOT);
  pulseLedInfo();
  updateLedStatus();

  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);

  // Button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButtonPress, FALLING);

  initRfidScanner(mfrc522);
  bleServiceBegin();
  bleSetCommandHandler(handleBleCommand);
  bleSetOtaCallback(onBleOtaData);

  settingsStoreReady = settingsStore.begin(SETTINGS_NS, false);
  if (settingsStoreReady) {
    loadSettingsFromStore();
    loadWifiSettingsFromStore();
    loadWebSettingsFromStore();
    Serial.print(F("[CFG] Loaded ")); Serial.println(buildSettingsPayload("CFG"));
    Serial.print(F("[WIFI] Loaded ")); Serial.println(buildWifiPayload("WIFI"));

    if (wifiAutoConnect && wifiSavedSsid.length() > 0) {
      startWifiConnect();
      setLedBaseState(LED_WIFI_CONNECTING);
    }
  } else {
    Serial.println(F("[CFG] Preferences init failed, using defaults"));
    pulseLedWarn();
  }

  byte version = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println(F("RC522 not found!"));
    setLedBaseState(LED_WIFI_ERROR);
    pulseLedError();
    while (1) { updateLedStatus(); delay(8); }
  }

  webPublishBegin(webServerUrl, webGameID, boardID, webEnabled);

  // Board ID = full MAC
  {
    bool needsNewId = boardID.length() == 0 || boardID.indexOf('-') < 0;
    if (needsNewId) {
      uint8_t mac[6];
      esp_read_mac(mac, ESP_MAC_WIFI_STA);
      char buf[18];
      snprintf(buf, sizeof(buf), "%02x-%02x-%02x-%02x-%02x-%02x",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      boardID = String(buf);
      if (settingsStoreReady) settingsStore.putString(KEY_BOARD_ID, boardID);
      Serial.print(F("[REG] boardID=")); Serial.println(boardID);
    }
  }

  boardRegBegin(webServerUrl, boardID, webGameID);
  boardRegSetGameIDCallback(onBoardRegGameIDAssigned);

  initStandardBoard(board);
  pieceDBCount = 0;
  whiteTurn = true;
  moveSeq = 0;

  for (int i = 0; i < NUM_ANTENNAS; i++) {
    squareUID[i] = "";
  }

  Serial.println(F("[BOOT] System ready."));
  Serial.println(F("[BOOT] Press button (GPIO 0) or send SNAPSHOT to scan + publish FEN."));
  printHelp();
  setLedBaseState(LED_IDLE);
  updateLedStatus();
}

// ---------------------------------------------------------------------------
// smartChessTick
// ---------------------------------------------------------------------------
void smartChessTick() {
  bleServicePoll();
  processSerialCommands();

  // ── Button debounce ──────────────────────────────────────────────
  if (buttonInterruptFlag) {
    noInterrupts();
    buttonInterruptFlag = false;
    interrupts();
    unsigned long now = millis();
    if (now - lastButtonMs > 50) {
      lastButtonMs = now;
      scanAllAndPublish();
    }
  }

  // ── WiFi state machine ───────────────────────────────────────────
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
      wifiRetryScheduledMs = 0;
      startWifiConnect();
      pulseLedWarn();
    }
  }

  // ── LED + subsystems ─────────────────────────────────────────────
  refreshLedBaseState();
  updateLedStatus();

  esp_task_wdt_delete(NULL);
  webPublishPoll();
  boardRegTick();
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();

  // ── Continuous scan print (diagnostic) ───────────────────────────
  if (scanContinuous) {
    for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
      scanBuf[idx] = squareUID[idx];
    }
    unsigned long now = millis();
    if (now - lastScanPrintMs >= SCAN_PRINT_INTERVAL_MS) {
      printScanCompact(scanBuf, scanVerbose, board, scanUsePieceLetters);
      lastScanPrintMs = now;
    }
  }

  esp_task_wdt_reset();
  delay(1);
}
