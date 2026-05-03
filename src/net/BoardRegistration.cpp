#include "net/BoardRegistration.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace {

constexpr unsigned long INTERVAL_NORMAL_MS = 30000;  // 30 s — during game
constexpr unsigned long INTERVAL_FAST_MS   =  5000;  //  5 s — waiting for game start

static String gServerUrl;
static String gBoardID;
static String gCurrentGameID;
static unsigned long gLastSentMs   = 0;
static bool          gFastPoll     = false;

static BoardRegGameIDCallback  gGameIDCallback  = nullptr;
static BoardRegCommandCallback gCommandCallback = nullptr;

// One-shot scan result queued by boardRegQueueScanResult()
static String gScanResult;   // "STARTED" | "MISSING" | "DUPLICATE" | ""
static String gScanDetail;   // comma-separated squares or ""

// One-shot in-game alert queued by boardRegQueueAlert()
static String gAlertCode;    // e.g. "WRONG_TURN", "PIECE_LOST"
static String gAlertDetail;  // human-readable context string

// ---------------------------------------------------------------------------
// Simple JSON string-field extractor — avoids a JSON library dependency.
// ---------------------------------------------------------------------------
static String extractJsonString(const String &json, const String &field) {
    String search = "\"" + field + "\":\"";
    int start = json.indexOf(search);
    if (start < 0) return "";
    start += search.length();
    int end = json.indexOf('"', start);
    if (end < 0) return "";
    return json.substring(start, end);
}

// ---------------------------------------------------------------------------
// POST /boards/scan-result — one-shot, clears gScanResult after sending.
// ---------------------------------------------------------------------------
static void doScanResult() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (gServerUrl.length() == 0 || gBoardID.length() == 0) return;

    String url = gServerUrl;
    if (!url.endsWith("/")) url += "/";
    url += "boards/scan-result";

    String body = "{\"boardID\":\"";
    body += gBoardID;
    body += "\",\"gameID\":\"";
    body += gCurrentGameID;
    body += "\",\"result\":\"";
    body += gScanResult;
    body += "\",\"detail\":\"";
    body += gScanDetail;
    body += "\"}";

    // Save in case we need to re-queue on network error
    String savedResult = gScanResult;
    String savedDetail = gScanDetail;

    // Clear now — restored below only on network failure
    gScanResult = "";
    gScanDetail = "";

    WiFiClientSecure wc;
    wc.setInsecure();  // skip cert validation — OK for internal board→server traffic
    HTTPClient http;
    if (!http.begin(wc, url)) {
        // Can't even start — re-queue for next tick
        gScanResult = savedResult;
        gScanDetail = savedDetail;
        Serial.println(F("[REG] scan-result http.begin failed, will retry"));
        return;
    }
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);
    int code = http.POST(body);
    http.end();
    Serial.printf("[REG] scan-result HTTP %d\n", code);

    if (code < 0) {
        // Network/TLS error — re-queue so it retries on next boardRegTick()
        gScanResult = savedResult;
        gScanDetail = savedDetail;
        Serial.println(F("[REG] scan-result network error, will retry"));
    }
    // 4xx/5xx: server received but rejected — don't retry
}

// ---------------------------------------------------------------------------
// POST /boards/alert — one-shot, clears gAlertCode after sending.
// ---------------------------------------------------------------------------
static void doAlert() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (gServerUrl.length() == 0 || gBoardID.length() == 0 || gCurrentGameID.length() == 0) return;

    String url = gServerUrl;
    if (!url.endsWith("/")) url += "/";
    url += "boards/alert";

    String body = "{\"boardID\":\"";
    body += gBoardID;
    body += "\",\"gameID\":\"";
    body += gCurrentGameID;
    body += "\",\"code\":\"";
    body += gAlertCode;
    body += "\",\"detail\":\"";
    body += gAlertDetail;
    body += "\"}";

    // Clear before sending
    gAlertCode   = "";
    gAlertDetail = "";

    WiFiClientSecure wc;
    wc.setInsecure();
    HTTPClient http;
    if (!http.begin(wc, url)) return;
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(3000);
    int code = http.POST(body);
    http.end();
    Serial.printf("[REG] alert %s HTTP %d\n", body.c_str(), code);
}

// ---------------------------------------------------------------------------
// POST /boards/heartbeat
// ---------------------------------------------------------------------------
static bool doHeartbeat() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (gServerUrl.length() == 0 || gBoardID.length() == 0) return false;

    String url = gServerUrl;
    if (!url.endsWith("/")) url += "/";
    url += "boards/heartbeat";

    String ip   = WiFi.localIP().toString();
    String body = "{\"boardID\":\"";
    body += gBoardID;
    body += "\",\"gameID\":\"";
    body += gCurrentGameID;
    body += "\",\"ip\":\"";
    body += ip;
    body += "\"}";

    WiFiClientSecure wc;
    wc.setInsecure();
    HTTPClient http;

    if (!http.begin(wc, url)) {
        Serial.println(F("[REG] http.begin failed"));
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    int code = http.POST(body);
    if (code < 200 || code >= 300) {
        http.end();
        Serial.printf("[REG] heartbeat error HTTP %d\n", code);
        return false;
    }

    String resp = http.getString();
    http.end();

    // ── gameID assignment (apply BEFORE logging so the log shows final value) ──
    String assignedGame = extractJsonString(resp, "gameID");
    if (assignedGame.length() > 0 && assignedGame != gCurrentGameID) {
        Serial.printf("[REG] gameID updated: %s -> %s\n",
                      gCurrentGameID.c_str(), assignedGame.c_str());
        gCurrentGameID = assignedGame;
        if (gGameIDCallback != nullptr) gGameIDCallback(assignedGame);
    }

    Serial.printf("[REG] heartbeat ok (%d) board=%s game=%s\n",
                  code, gBoardID.c_str(), gCurrentGameID.c_str());

    // ── command from server ──────────────────────────────────────────────
    String command = extractJsonString(resp, "command");
    if (command.length() > 0 && gCommandCallback != nullptr) {
        Serial.print(F("[REG] server command="));
        Serial.println(command);
        gCommandCallback(command);
    }

    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void boardRegBegin(const String &serverUrl,
                   const String &boardID,
                   const String &gameID) {
    gServerUrl     = serverUrl;
    gBoardID       = boardID;
    gCurrentGameID = gameID;
    gLastSentMs    = 0;  // fire first heartbeat immediately

    Serial.print(F("[REG] Init board="));
    Serial.print(gBoardID);
    Serial.print(F(" url="));
    Serial.println(gServerUrl);
}

void boardRegSetGameID(const String &gameID) {
    gCurrentGameID = gameID;
}

void boardRegSetConfig(const String &serverUrl, const String &boardID) {
    gServerUrl = serverUrl;
    gBoardID   = boardID;
}

void boardRegSetGameIDCallback(BoardRegGameIDCallback cb)  { gGameIDCallback  = cb; }
void boardRegSetCommandCallback(BoardRegCommandCallback cb) { gCommandCallback = cb; }

void boardRegSetFastPoll(bool fast) { gFastPoll = fast; }

void boardRegForceHeartbeat() { gLastSentMs = 0; }

void boardRegQueueScanResult(const String &result, const String &detail) {
    gScanResult = result;
    gScanDetail = detail;
}

void boardRegQueueAlert(const String &code, const String &detail) {
    gAlertCode   = code;
    gAlertDetail = detail;
}

void boardRegTick() {
    if (gServerUrl.length() == 0 || gBoardID.length() == 0) return;
    if (WiFi.status() != WL_CONNECTED) return;

    // Send queued scan result immediately (before next heartbeat)
    if (gScanResult.length() > 0) {
        doScanResult();
    }

    // Send queued in-game alert immediately
    if (gAlertCode.length() > 0) {
        doAlert();
    }

    unsigned long interval = gFastPoll ? INTERVAL_FAST_MS : INTERVAL_NORMAL_MS;
    if (millis() - gLastSentMs < interval) return;

    gLastSentMs = millis();
    doHeartbeat();
}
