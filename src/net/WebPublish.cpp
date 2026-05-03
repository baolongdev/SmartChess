#include "net/WebPublish.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

namespace {

enum WebPublishState {
    WEB_IDLE = 0,
    WEB_QUEUED,
    WEB_SENDING,
    WEB_DONE,
    WEB_ERROR,
};

static String   gServerUrl   = "";
static String   gGameID      = "";
static bool     gEnabled     = false;

static WebPublishState gState       = WEB_IDLE;
static String          gPendingUci  = "";
static int             gPendingSeq  = 0;
static String          gLastError   = "";
static int             gLastStatus  = 0;

// ---------------------------------------------------------------------------
// Perform the HTTP POST synchronously.  Returns true on 2xx response.
// ---------------------------------------------------------------------------
static bool doPost(const String &uci, int seq) {
    if (WiFi.status() != WL_CONNECTED) {
        gLastError = "WIFI_NOT_CONNECTED";
        return false;
    }

    if (gServerUrl.length() == 0 || gGameID.length() == 0) {
        gLastError = "NO_CONFIG";
        return false;
    }

    String url = gServerUrl;
    if (!url.endsWith("/")) url += "/";
    url += "moves";

    // Build JSON body manually — no external library needed
    String body = "{\"uci\":\"";
    body += uci;
    body += "\",\"gameID\":\"";
    body += gGameID;
    body += "\",\"seq\":";
    body += String(seq);
    body += "}";

    WiFiClientSecure client;
    client.setInsecure();  // skip cert validation — OK for internal board→server traffic
    HTTPClient http;

    if (!http.begin(client, url)) {
        gLastError = "HTTP_BEGIN_FAILED";
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);  // 5 s — generous for LAN

    int statusCode = http.POST(body);
    String respBody = http.getString();
    http.end();

    gLastStatus = statusCode;

    if (statusCode < 200 || statusCode >= 300) {
        gLastError = String("HTTP_") + String(statusCode);
        Serial.print(F("[WEB] POST failed status="));
        Serial.print(statusCode);
        Serial.print(F(" body="));
        Serial.println(respBody.substring(0, 80));
        return false;
    }

    Serial.print(F("[WEB] POST ok uci="));
    Serial.print(uci);
    Serial.print(F(" seq="));
    Serial.print(seq);
    Serial.print(F(" status="));
    Serial.println(statusCode);

    gLastError = "";
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void webPublishBegin(const String &serverUrl, const String &gameID, bool enabled) {
    gServerUrl  = serverUrl;
    gGameID     = gameID;
    gEnabled    = enabled;
    gState      = WEB_IDLE;
    gLastError  = "";
    gLastStatus = 0;

    Serial.print(F("[WEB] Init url="));
    Serial.print(gServerUrl);
    Serial.print(F(" game="));
    Serial.print(gGameID);
    Serial.print(F(" enabled="));
    Serial.println(gEnabled ? F("YES") : F("NO"));
}

void webPublishSetConfig(const String &serverUrl, const String &gameID, bool enabled) {
    gServerUrl = serverUrl;
    gGameID    = gameID;
    gEnabled   = enabled;
}

void webPublishMove(const String &uci, int seq) {
    if (!gEnabled) return;

    // Drop if a job is already in flight — the server handles dedup via seq
    if (gState == WEB_QUEUED || gState == WEB_SENDING) {
        Serial.println(F("[WEB] Busy, dropping move (will retry on next)"));
        return;
    }

    gPendingUci = uci;
    gPendingSeq = seq;
    gState      = WEB_QUEUED;
}

void webPublishPoll() {
    if (!gEnabled || gState != WEB_QUEUED) return;

    gState = WEB_SENDING;
    bool ok = doPost(gPendingUci, gPendingSeq);
    gState  = ok ? WEB_DONE : WEB_ERROR;
}

bool webPublishIsEnabled() {
    return gEnabled;
}

const String& webPublishGetServerUrl() {
    return gServerUrl;
}

const String& webPublishGetGameID() {
    return gGameID;
}

String webPublishStatusPayload(const char *prefix) {
    String s = String(prefix);
    s += "|url=";
    s += gServerUrl.length() > 0 ? gServerUrl : "-";
    s += "|game=";
    s += gGameID.length() > 0 ? gGameID : "-";
    s += "|enabled=";
    s += gEnabled ? "1" : "0";
    s += "|state=";
    switch (gState) {
        case WEB_IDLE:    s += "IDLE";    break;
        case WEB_QUEUED:  s += "QUEUED";  break;
        case WEB_SENDING: s += "SENDING"; break;
        case WEB_DONE:    s += "DONE";    break;
        case WEB_ERROR:   s += "ERROR";   break;
    }
    if (gLastError.length() > 0) {
        s += "|err=";
        s += gLastError;
    }
    return s;
}
