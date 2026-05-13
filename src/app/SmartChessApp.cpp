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
#include "utils/Logger.h"
#include "net/WebPublish.h"
#include "net/BoardRegistration.h"

#include <esp_ota_ops.h>
#include <esp_system.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// Button — GPIO 38 (pull-up, active LOW, state-machine debounce, no ISR)
// ---------------------------------------------------------------------------
constexpr int BUTTON_PIN = 38;
static unsigned long btnLastPressMs = 0;      // millis() of last accepted press
constexpr unsigned long BTN_COOLDOWN_MS = 35; // min gap between presses
enum { BTN_IDLE, BTN_DEBOUNCE, BTN_WAIT_RELEASE } btnState = BTN_IDLE;
static unsigned long btnDebounceUs = 0;         // micros() when debounce started

// ---------------------------------------------------------------------------
// UART2 — hardware serial for external controller (TX=19, RX=20)
// ---------------------------------------------------------------------------
constexpr int UART2_TX = 19;
constexpr int UART2_RX = 20;

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
static bool gameStarted = false;

// Async START verification — scan happens in loop(), not in heartbeat handler
static bool pendingStartScan = false;
static unsigned long startRetryBlockedUntilMs = 0;

// ---------------------------------------------------------------------------
// Scan settings — persisted in NVS
// ---------------------------------------------------------------------------
static bool scanVerbose = false;
static bool scanContinuous = false;
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
  logI("WIFI", String("CONNECTING ssid=") + wifiSavedSsid);
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
  scanContinuous = settingsStore.getBool(KEY_SCAN_C, false);
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

static void scanAllSquares();  // forward decl — defined below

// ── Multi-read majority-LEARN + post-verification ─────────────────────────
// For each square, reads the UID 3 times and accepts only if ≥2/3 match
// (filters intermittent crosstalk).  Then checks for duplicate UIDs across
// squares (persistent crosstalk); duplicates are cleared and retried.
// After registration, re-scans the entire board and verifies the resulting
// position matches the standard starting position.  If not, the entire
// process is retried (up to LEARN_TOTAL_ATTEMPTS).
// Returns a loggable status string like "LEARNED pieces=32/32".
// ──────────────────────────────────────────────────────────────────────────

static constexpr int LEARN_READS_PER_SQUARE = 5;   // must get ≥3 same
static constexpr int LEARN_READS_THRESHOLD  = 3;
static constexpr int LEARN_MAX_RETRIES      = 5;    // collision-retry passes
static constexpr int LEARN_TOTAL_ATTEMPTS   = 3;    // full LEARN retries
static constexpr unsigned long START_RETRY_COOLDOWN_MS = 10000;

// Standard starting position for post-LEARN validation
static const char LEARN_STD_PIECES[] = "RNBQKBNR";
static const char LEARN_STD_PAWNS[]  = "PPPPPPPP";
static const char LEARN_STDb_PIECES[] = "rnbqkbnr";
static const char LEARN_STDb_PAWNS[]  = "pppppppp";

// Read one square multiple times; return UID that appears ≥ READ_THRESHOLD
static String majorityRead(int idx, int reads, int threshold) {
  struct { String uid; int count; } cand[4];
  int n = 0;
  for (int a = 0; a < reads; a++) {
    String uid = scanUID(mfrc522, idx);
    delayMicroseconds(300);
    if (uid.length() == 0) continue;
    int found = -1;
    for (int c = 0; c < n; c++) { if (cand[c].uid == uid) { found = c; break; } }
    if (found >= 0) {
      cand[found].count++;
    } else if (n < 4) {
      cand[n].uid = uid; cand[n].count = 1; n++;
    }
  }
  for (int c = 0; c < n; c++) {
    if (cand[c].count >= threshold) return cand[c].uid;
  }
  return "";
}

static String learnPieces() {
  char stdBoard[8][8];
  initStandardBoard(stdBoard);   // saved for validation later
  pieceDBCount = 0;

  for (int attempt = 0; attempt < LEARN_TOTAL_ATTEMPTS; attempt++) {
    String detected[NUM_ANTENNAS];
    bool   resolved[NUM_ANTENNAS] = {false};

    // Phase 1: majority-vote read each square
    for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
      String uid = majorityRead(idx, LEARN_READS_PER_SQUARE, LEARN_READS_THRESHOLD);
      if (uid.length() > 0) { detected[idx] = uid; resolved[idx] = true; }
    }

    // Phase 2: resolve duplicates (persistent crosstalk) via retry
    for (int rp = 0; rp < LEARN_MAX_RETRIES; rp++) {
      bool conflict = false;
      // find duplicates
      for (int i = 0; i < NUM_ANTENNAS; i++) {
        if (!resolved[i] || detected[i].length() == 0) continue;
        for (int j = i + 1; j < NUM_ANTENNAS; j++) {
          if (!resolved[j] || detected[j].length() == 0) continue;
          if (detected[j] == detected[i]) {
            resolved[i] = false; resolved[j] = false; conflict = true;
          }
        }
      }
      if (!conflict) break;
      // re-scan cleared squares with longer settle
      for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
        if (resolved[idx]) continue;
        delayMicroseconds(600);
        String uid = majorityRead(idx, LEARN_READS_PER_SQUARE, LEARN_READS_THRESHOLD);
        if (uid.length() == 0) continue;
        bool claimed = false;
        for (int k = 0; k < NUM_ANTENNAS; k++) {
          if (k != idx && resolved[k] && detected[k] == uid) { claimed = true; break; }
        }
        if (!claimed) { detected[idx] = uid; resolved[idx] = true; }
      }
    }

    // Phase 3: register
    pieceDBCount = 0;
    int learned = 0;
    for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
      if (resolved[idx] && detected[idx].length() > 0) {
        int file = idx / 8;
        int rank = idx % 8;
        char piece = stdBoard[file][rank];
        if (piece != '.') { registerPiece(detected[idx], piece); learned++; }
      }
    }

    if (learned < 32) {
      logW("LEARN", String("Incomplete LEARN (attempt ") + (attempt + 1)
             + "/" + LEARN_TOTAL_ATTEMPTS + "), retrying...");
      pieceDBCount = 0;
      continue;
    }

    // Phase 4: VERIFICATION — validate piece-type counts.
    // A correct standard-position registration must have exactly:
    //   White: 1K 1Q 2R 2B 2N 8P
    //   Black: 1k 1q 2r 2b 2n 8p
    // If counts don't add up (e.g. queen UID registered as 'P' on a2,
    // then also as 'Q' on d1 → total queens=2, pawns=7), the LEARN
    // is corrupt and must be retried.
    int counts[256] = {0};
    for (int i = 0; i < pieceDBCount; i++) {
      counts[(unsigned char)pieceDB[i].piece]++;
    }
    auto expectCount = [&](char piece, int expected) -> bool {
      return counts[(unsigned char)piece] == expected;
    };
    bool valid = true;
    valid = valid && expectCount('K', 1) && expectCount('Q', 1);
    valid = valid && expectCount('R', 2) && expectCount('B', 2) && expectCount('N', 2);
    valid = valid && expectCount('P', 8);
    valid = valid && expectCount('k', 1) && expectCount('q', 1);
    valid = valid && expectCount('r', 2) && expectCount('b', 2) && expectCount('n', 2);
    valid = valid && expectCount('p', 8);

    if (valid) {
      String msg = String("LEARNED pieces=") + learned + "/32";
      logI("LEARN", msg);
      return msg;
    }
    // Verification failed — retry entire LEARN
    logW("LEARN", String("Verification FAILED (attempt ") + (attempt + 1)
           + "/" + LEARN_TOTAL_ATTEMPTS + "), retrying...");
    pieceDBCount = 0;
  }

  // All attempts exhausted — register whatever we have
  pieceDBCount = 0;
  logE("LEARN", "All attempts exhausted — LEARN failed");
  return "LEARN FAILED after " + String(LEARN_TOTAL_ATTEMPTS) + " attempts";
}

// ---------------------------------------------------------------------------
// FEN builder wrapper
// ---------------------------------------------------------------------------
static char fenBuffer[128];
static const char* buildCurrentFen() {
  fenBuild(board, fenBuffer, sizeof(fenBuffer),
           whiteTurn,
           -1, -1,
           0,
           moveSeq + 1,
           false, false, false, false, false, false);
  return fenBuffer;
}

static const char* buildPlyFen(bool turn, int fullmove) {
  fenBuild(board, fenBuffer, sizeof(fenBuffer),
           turn,
           -1, -1,
           0,
           fullmove,
           false, false, false, false, false, false);
  return fenBuffer;
}

// ---------------------------------------------------------------------------
// scanAllAndPublish — 1 press = 1 ply (professional tournament standard)
// - whiteTurn tells us who just moved
// - FEN shows next player to move, fullmove increments after Black's move
// ---------------------------------------------------------------------------
static void scanAllSquares() {
  for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
    String uid = scanUID(mfrc522, idx);
    int file = idx / 8;
    int rank = idx % 8;
    if (uid.length() == 0) {
      board[file][rank] = '.';
      squareUID[idx] = "";
    } else {
      char piece = lookupUID(uid);
      board[file][rank] = (piece == '?') ? '?' : piece;
      squareUID[idx] = uid;
    }
    if (idx % 8 == 0) esp_task_wdt_reset();
  }
}

static void majorityReadAllSquares() {
  // Read each square 3 times, accept UID that appears ≥2 (filters intermittent crosstalk).
  // ~3× slower than single scan but much more robust.
  struct { String uid; int count; } tally[3];
  for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
    int n = 0;
    for (int r = 0; r < 3; r++) {
      String uid = scanUID(mfrc522, idx);
      int found = -1;
      for (int c = 0; c < n; c++) { if (tally[c].uid == uid) { found = c; break; } }
      if (found >= 0) {
        tally[found].count++;
      } else if (n < 3) {
        tally[n].uid = uid; tally[n].count = 1; n++;
      }
    }
    // pick majority UID (≥2)
    String bestUid = "";
    for (int c = 0; c < n; c++) { if (tally[c].count >= 2) { bestUid = tally[c].uid; break; } }
    int file = idx / 8;
    int rank = idx % 8;
    if (bestUid.length() == 0) {
      board[file][rank] = '.';
      squareUID[idx] = "";
    } else {
      char piece = lookupUID(bestUid);
      board[file][rank] = (piece == '?') ? '?' : piece;
      squareUID[idx] = bestUid;
    }
    if (idx % 8 == 0) esp_task_wdt_reset();
  }
}

static void scanAllAndPublish() {
  pulseLedInfo();
  unsigned long scanStart = millis();
  scanAllSquares();

  if (!gameStarted) {
    unsigned long elapsed = millis() - scanStart;
    logI("SCAN", String("Board scanned ") + elapsed + "ms");
    printBoard(board);
    logI("SCAN", "Game not started — board display only");
    pulseLedOk();
    return;
  }

  // Fullmove increments after Black's move (standard chess)
  if (!whiteTurn) moveSeq++;

  bool nextTurn = !whiteTurn;
  int fullmove = moveSeq + 1;
  const char *fen = buildPlyFen(nextTurn, fullmove);

  unsigned long elapsed = millis() - scanStart;
  const char *player = whiteTurn ? "White" : "Black";
  logI("SCAN", String(player) + " just moved | "
       + elapsed + "ms | " + fen);
  printBoard(board);
  webPublishFEN(String(fen), fullmove);
  bleUpdateFEN(String(fen));

  whiteTurn = nextTurn;
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
  logI("CFG", String("SAVED ") + response);
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
  logI("WIFI", String("SAVED ") + response);
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

// Forward declarations
static bool handleCommonCommand(const String &cmd, const String &cmdUpper,
                                String &response, bool &handled);

// ---------------------------------------------------------------------------
// BoardRegistration callbacks
// ---------------------------------------------------------------------------
static void onBoardRegGameIDAssigned(const String &newGameID) {
  webGameID = newGameID;
  webPublishSetConfig(webServerUrl, webGameID, boardID, webEnabled);
  persistWebSettingsToStore();
  logI("REG", String("gameID assigned=") + newGameID);
}

// ---------------------------------------------------------------------------
// executeStartScan — scan RFID, validate standard position, start or reject
// ---------------------------------------------------------------------------
static void executeStartScan(String &outResponse) {
  // Auto-learn if no piece UIDs have been learned yet (e.g. after FACTORY_RESET)
  if (pieceDBCount == 0) {
    learnPieces();
    logI("START", "Auto-learn complete");
  }

  scanAllSquares();

  // Standard starting position:
  //   Rank 1 (r=0): R N B Q K B N R
  //   Rank 2 (r=1): P P P P P P P P
  //   Ranks 3-6:    . . . . . . . .
  //   Rank 7 (r=6): p p p p p p p p
  //   Rank 8 (r=7): r n b q k b n r
  static const char STD_PIECES[] = "RNBQKBNR";
  static const char STD_PAWNS[]  = "PPPPPPPP";
  static const char STDb_PIECES[] = "rnbqkbnr";
  static const char STDb_PAWNS[]  = "pppppppp";

  String wrongSq = "";
  String missingSq = "";
  for (int f = 0; f < 8; f++) {
    for (int r = 0; r < 8; r++) {
      char expected = '.';
      if (r == 0)      expected = STD_PIECES[f];
      else if (r == 1) expected = STD_PAWNS[f];
      else if (r == 6) expected = STDb_PAWNS[f];
      else if (r == 7) expected = STDb_PIECES[f];

      char actual = board[f][r];
      String sq = squareNameFromIdx(squareIdx(f, r));

      if (expected != '.' && (actual == '.' || actual == '?')) {
        if (missingSq.length() > 0) missingSq += ",";
        missingSq += sq;
      } else if (expected == '.' && actual != '.' && actual != '?') {
        if (wrongSq.length() > 0) wrongSq += ",";
        wrongSq += sq;
      } else if (expected != '.' && actual != '.' && actual != '?' && expected != actual) {
        if (wrongSq.length() > 0) wrongSq += ",";
        wrongSq += sq;
      }
    }
  }

  if (wrongSq.length() == 0 && missingSq.length() == 0) {
    if (btnState != BTN_IDLE) {
      boardRegQueueScanResult("MISSING", "BUTTON_STATE_FAIL");
      boardRegSetFastPoll(true);
      boardRegForceHeartbeat();
      logW("GAME", "START rejected — BUTTON_STATE_FAIL");
      outResponse = "GAME_ERR:BUTTON_STATE_FAIL";
      pulseLedError();
      return;
    }

    gameStarted = true;
    whiteTurn = true; // always White to move first at game start
    moveSeq = 0;
    btnState = BTN_IDLE;
    btnLastPressMs = 0;
    boardRegQueueScanResult("STARTED", "");
    boardRegSetFastPoll(false);
    boardRegForceHeartbeat();
    logI("GAME", String("START — board verified, ") + pieceDBCount + " UIDs mapped. White moves first.");
    outResponse = "GAME_STARTED";
    pulseLedOk();
  } else {
    String resultType;
    String detail;
    if (missingSq.length() > 0) {
      resultType = "MISSING";
      detail = missingSq;
      if (wrongSq.length() > 0) detail += "," + wrongSq;
    } else {
      resultType = "DUPLICATE";
      detail = wrongSq;
    }
    boardRegQueueScanResult(resultType, detail);
    logW("GAME", String("START rejected — ") + resultType + " " + detail);
    outResponse = "GAME_ERR:" + resultType + ":" + detail;
    pulseLedError();
  }
}

/** Dispatch START/STOP commands received via heartbeat response from server. */
static void onBoardRegCommand(const String &command) {
  String cmdUpper = command;
  cmdUpper.toUpperCase();
  logI("CMD", String("Server command: ") + command);

  if (cmdUpper == "START") {
    pendingStartScan = true;
  } else if (cmdUpper == "STOP") {
    gameStarted = false;
    btnState = BTN_IDLE;
    btnLastPressMs = 0;
    logI("GAME", "STOP from server");
  } else {
    logW("CMD", String("Unhandled server command: ") + command);
  }
}

// ---------------------------------------------------------------------------
// FACTORY_RESET — clear all NVS, reset board, restart
// ---------------------------------------------------------------------------
static void doFactoryReset() {
  logW("RESET", "Factory reset — clearing all settings...");
  WiFi.disconnect(true);
  if (settingsStoreReady) {
    settingsStore.clear();
    settingsStore.end();
  }
  delay(100);
  logW("RESET", "NVS cleared. Restarting...");
  delay(200);
  esp_restart();
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

  if (cmdUpper == "START") {
    handled = true;
    executeStartScan(response);
    return true;
  }

  if (cmdUpper == "STOP") {
    handled = true;
    gameStarted = false;
    btnState = BTN_IDLE;
    btnLastPressMs = 0;
    logI("GAME", "STOP — game ended.");
    response = "GAME_STOPPED";
    pulseLedInfo();
    return true;
  }

  if (cmdUpper == "FACTORY_RESET") {
    handled = true; doFactoryReset(); return true;
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
    logI("BTN", "SNAPSHOT command -> scanAllAndPublish()");
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
    response = learnPieces();
    whiteTurn = true;
    moveSeq = 0;
    pulseLedOk();
    return true;
  }

  bool handled = false;
  if (handleCommonCommand(cmd, cmdUpper, response, handled)) return true;
  if (handled) return false;

  response = "UNKNOWN_CMD";
  logW("CMD", String("Unknown: ") + cmdUpper);
  pulseLedError();
  return false;
}

// ---------------------------------------------------------------------------
// Serial command handler
// ---------------------------------------------------------------------------
static String cmdBuffer = "";
static unsigned long cmdLastByteMs = 0;
static String cmdBuffer2 = "";
static unsigned long cmdLastByteMs2 = 0;
static constexpr unsigned long CMD_COMMIT_IDLE_MS = 120;

static void printHelp() {
  Serial.println(F("+----------------+----------------------------------------+"));
  Serial.println(F("| SmartChess CMD | Description                            |"));
  Serial.println(F("+----------------+----------------------------------------+"));
  Serial.println(F("| START          | Begin new game — reset + notify server |"));
  Serial.println(F("| STOP           | End current game — reset state         |"));
  Serial.println(F("| SNAPSHOT       | Scan 2 plies + publish FEN (full move) |"));
  Serial.println(F("| STATUS         | Board / WiFi / BLE status              |"));
  Serial.println(F("| F  | FEN       | Print current FEN                      |"));
  Serial.println(F("| LEARN          | Scan + learn 32 piece UIDs             |"));
  Serial.println(F("+----------------+----------------------------------------+"));
  Serial.println(F("| CFG            | Read settings                          |"));
  Serial.println(F("| CFG_SET|...    | Write settings (VERBOSE,CONTINUOUS,...)|"));
  Serial.println(F("+----------------+----------------------------------------+"));
  Serial.println(F("| WIFI           | WiFi status                            |"));
  Serial.println(F("| WIFI_SET|...   | Configure WiFi (SSID,PASS,AUTO)        |"));
  Serial.println(F("| WIFI_SCAN      | Scan available networks                |"));
  Serial.println(F("| WIFI_CONNECT   | Connect to saved WiFi                  |"));
  Serial.println(F("| WIFI_DISCONNECT| Disconnect WiFi                        |"));
  Serial.println(F("+----------------+----------------------------------------+"));
  Serial.println(F("| WEB            | Web publish config                     |"));
  Serial.println(F("| WEB_SET|...    | Configure web (URL,GAME,BOARD,ENABLED) |"));
  Serial.println(F("+----------------+----------------------------------------+"));
  Serial.println(F("| FACTORY_RESET  | Clear ALL settings + restart board     |"));
  Serial.println(F("| HELP           | This help                              |"));
  Serial.println(F("+----------------+----------------------------------------+"));
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
    logI("BTN", "SNAPSHOT (serial) -> scanAllAndPublish()");
    scanAllAndPublish(); return;
  }
  if (cmdUpper == "STATUS") {
    pulseLedInfo();
    logI("STATE", String("turn=") + (whiteTurn ? "WHITE" : "BLACK")
         + " seq=" + moveSeq + " pieces=" + pieceDBCount);
    return;
  }
  if (cmdUpper == "F" || cmdUpper == "FEN") {
    logI("FEN", buildCurrentFen());
    return;
  }
  if (cmdUpper == "LEARN") {
    learnPieces();
    whiteTurn = true;
    moveSeq = 0;
    return;
  }

  String response; bool handled = false;
  handleCommonCommand(cmd, cmdUpper, response, handled);
    if (handled) {
      if (response.startsWith("WIFI_ERR") || response.startsWith("CFG_ERR")) {
        pulseLedError();
        logW("CMD", response);
      } else {
        pulseLedInfo();
        logI("CMD", response);
      }
      return;
    }

    logW("CMD", String("Unknown: ") + cmd);
    pulseLedError();
}

static void processSerialInput(HardwareSerial &port, String &buf, unsigned long &lastMs) {
  while (port.available()) {
    char c = (char)port.read();
    if (c == '\r' || c == '\n') {
      if (buf.length() > 0) {
        handleCommand(buf);
        buf = ""; lastMs = 0;
      }
      continue;
    }
    if (buf.length() < 256) { buf += c; lastMs = millis(); }
  }
  if (buf.length() > 0 && lastMs > 0 &&
      millis() - lastMs >= CMD_COMMIT_IDLE_MS) {
    handleCommand(buf);
    buf = ""; lastMs = 0;
  }
}

// ---------------------------------------------------------------------------
// smartChessBegin
// ---------------------------------------------------------------------------
void smartChessBegin() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 3000) {}
  logInit();

#if defined(RGB_BUILTIN)
  pinMode(RGB_BUILTIN, OUTPUT);
#endif
  setLedBaseState(LED_BOOT);
  pulseLedInfo();
  updateLedStatus();

  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);

  // Button (pull-up, polled in smartChessTick — no ISR)
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // UART2 — hardware serial for external controller
  Serial2.begin(115200, SERIAL_8N1, UART2_RX, UART2_TX);
  logSetSerial2(&Serial2);

  initRfidScanner(mfrc522);
  bleServiceBegin();
  bleSetCommandHandler(handleBleCommand);
  bleSetOtaCallback(onBleOtaData);

  settingsStoreReady = settingsStore.begin(SETTINGS_NS, false);
  if (settingsStoreReady) {
    loadSettingsFromStore();
    loadWifiSettingsFromStore();
    loadWebSettingsFromStore();
    logI("CFG", String("Loaded ") + buildSettingsPayload("CFG"));
    logI("WIFI", String("Loaded ") + buildWifiPayload("WIFI"));

    if (wifiAutoConnect && wifiSavedSsid.length() > 0) {
      startWifiConnect();
      setLedBaseState(LED_WIFI_CONNECTING);
    }
  } else {
    logW("CFG", "Preferences init failed, using defaults");
    pulseLedWarn();
  }

  byte version = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if (version == 0x00 || version == 0xFF) {
    logE("HW", "RC522 not found — HALT");
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
      logI("REG", String("boardID=") + boardID);
    }
  }

  boardRegBegin(webServerUrl, boardID, webGameID);
  boardRegSetGameIDCallback(onBoardRegGameIDAssigned);
  boardRegSetCommandCallback(onBoardRegCommand);

  initStandardBoard(board);
  pieceDBCount = 0;
  whiteTurn = true;
  moveSeq = 0;

  for (int i = 0; i < NUM_ANTENNAS; i++) {
    squareUID[i] = "";
  }

  logI("BOOT", "UART2 ready — TX=19 RX=20 115200 baud");
  logI("BOOT", "System ready.");
  logI("BOOT", "Press button (GPIO 38) or send SNAPSHOT to scan + publish FEN.");
  printHelp();
  setLedBaseState(LED_IDLE);
  updateLedStatus();
}

// ---------------------------------------------------------------------------
// smartChessTick
// ---------------------------------------------------------------------------
void smartChessTick() {
  bleServicePoll();
  processSerialInput(Serial, cmdBuffer, cmdLastByteMs);
  processSerialInput(Serial2, cmdBuffer2, cmdLastByteMs2);

  // ── Button (state-machine debounce, broadcasts DOWN/UP) ───────────
  {
    int reading = digitalRead(BUTTON_PIN);
    switch (btnState) {
      case BTN_IDLE:
        if (reading == LOW) {
          btnDebounceUs = micros();
          btnState = BTN_DEBOUNCE;
        }
        break;
      case BTN_DEBOUNCE:
        if (reading == HIGH) {
          btnState = BTN_IDLE;                       // noise, back to idle
        } else if (micros() - btnDebounceUs > 900) {
          unsigned long now = millis();
          if (now - btnLastPressMs > BTN_COOLDOWN_MS) {
            btnLastPressMs = now;
            bleLogImmediate("[BTN_DOWN]");
            logI("BTN", whiteTurn ? "den luot di cua Den" : "den luot di cua Trang");
            logI("BTN", "Pressed -> scanAllAndPublish()");
            boardRegSetBtnPressed();
            boardRegForceHeartbeat();
            scanAllAndPublish();
            btnState = BTN_WAIT_RELEASE;
          }
          // else cooldown active — discard
        }
        break;
      case BTN_WAIT_RELEASE:
        if (reading == HIGH) {
          btnState = BTN_IDLE;
          bleLogImmediate("[BTN_UP]");
          logI("BTN", whiteTurn ? "den luot di cua Den" : "den luot di cua Trang");
          logI("BTN", "Released -> scanAllAndPublish()");
          boardRegSetBtnPressed();
          boardRegForceHeartbeat();
          scanAllAndPublish();
        }
        break;
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
      logI("WIFI", String("CONNECTED ip=") + WiFi.localIP().toString());
      pulseLedOk();
    } else if (wifiStatus == WL_CONNECT_FAILED) {
      wifiConnectInProgress = false;
      wifiConnectFailed = true;
      wifiLastError = "AUTH";
      logE("WIFI", String("CONNECT_FAILED auth ssid=") + wifiSavedSsid);
      pulseLedError();
      if (wifiAutoConnect) wifiRetryScheduledMs = millis() + WIFI_RETRY_DELAY_MS;
    } else if (wifiStatus == WL_NO_SSID_AVAIL) {
      wifiConnectInProgress = false;
      wifiConnectFailed = true;
      wifiLastError = "NO_SSID";
      logE("WIFI", String("CONNECT_FAILED no_ssid ssid=") + wifiSavedSsid);
      pulseLedError();
      if (wifiAutoConnect) wifiRetryScheduledMs = millis() + WIFI_RETRY_DELAY_MS;
    } else if (millis() - wifiConnectStartMs > WIFI_CONNECT_TIMEOUT_MS) {
      wifiConnectInProgress = false;
      wifiConnectFailed = true;
      wifiLastError = "TIMEOUT";
      logE("WIFI", String("CONNECT_TIMEOUT ssid=") + wifiSavedSsid);
      pulseLedError();
      if (wifiAutoConnect) wifiRetryScheduledMs = millis() + WIFI_RETRY_DELAY_MS;
    }
  }

  if (wifiRetryScheduledMs > 0 && millis() >= wifiRetryScheduledMs) {
    wifiRetryScheduledMs = 0;
    if (!wifiConnectedNow && wifiAutoConnect && wifiSavedSsid.length() > 0) {
      logI("WIFI", String("AUTO_RETRY ssid=") + wifiSavedSsid);
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

  // ── Deferred START scan ───────────────────────────────────────────
  if (pendingStartScan) {
    pendingStartScan = false;
    String tmp;
    executeStartScan(tmp);
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
