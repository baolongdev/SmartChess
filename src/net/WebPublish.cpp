#include "net/WebPublish.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "utils/Logger.h"

namespace {

static bool httpBegin(HTTPClient &http, WiFiClient &plain, WiFiClientSecure &secure,
                      const String &url) {
    if (url.startsWith("https://")) {
        secure.setInsecure();
        return http.begin(secure, url);
    }
    return http.begin(plain, url);
}

enum WebPublishState {
    WEB_IDLE = 0,
    WEB_QUEUED,
    WEB_SENDING,
    WEB_DONE,
    WEB_ERROR,
};

static String   gServerUrl   = "";
static String   gGameID      = "";
static String   gBoardID     = "";
static bool     gEnabled     = false;

static WebPublishState gState       = WEB_IDLE;
static String          gPendingFen  = "";
static int             gPendingSeq  = 0;
static String          gLastError   = "";
static int             gLastStatus  = 0;

static bool doPost(const String &fen, int seq) {
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
    url += "fen";

    String body = "{\"fen\":\"";
    body += fen;
    body += "\",\"gameID\":\"";
    body += gGameID;
    body += "\",\"boardID\":\"";
    body += gBoardID;
    body += "\",\"seq\":";
    body += String(seq);
    body += "}";

    WiFiClient plain;
    WiFiClientSecure secure;
    HTTPClient http;

    if (!httpBegin(http, plain, secure, url)) {
        gLastError = "HTTP_BEGIN_FAILED";
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    int statusCode = http.POST(body);
    String respBody = http.getString();
    http.end();

    gLastStatus = statusCode;

    if (statusCode < 200 || statusCode >= 300) {
        gLastError = String("HTTP_") + String(statusCode);
        logW("FEN", String("POST failed status=") + statusCode + " body=" + respBody.substring(0, 80));
        return false;
    }

    logI("FEN", String("POST ok fen=") + fen.substring(0, 40) + " seq=" + seq + " status=" + statusCode);

    gLastError = "";
    return true;
}

}  // namespace

void webPublishBegin(const String &serverUrl, const String &gameID, const String &boardID, bool enabled) {
    gServerUrl  = serverUrl;
    gGameID     = gameID;
    gBoardID    = boardID;
    gEnabled    = enabled;
    gState      = WEB_IDLE;
    gLastError  = "";
    gLastStatus = 0;

    logI("FEN", String("Init url=") + gServerUrl + " game=" + gGameID + " board=" + gBoardID + " enabled=" + (gEnabled ? "YES" : "NO"));
}

void webPublishSetConfig(const String &serverUrl, const String &gameID, const String &boardID, bool enabled) {
    gServerUrl = serverUrl;
    gGameID    = gameID;
    gBoardID   = boardID;
    gEnabled   = enabled;
}

void webPublishFEN(const String &fen, int seq) {
    if (!gEnabled) return;

    if (gState == WEB_QUEUED || gState == WEB_SENDING) {
        logW("FEN", "Busy, dropping snapshot (will be overwritten by next)");
        return;
    }

    gPendingFen  = fen;
    gPendingSeq  = seq;
    gLastStatus  = 0;
    gState       = WEB_QUEUED;
}

int webPublishGetLastStatus() {
    return gLastStatus;
}

void webPublishPoll() {
    if (!gEnabled || gState != WEB_QUEUED) return;

    gState = WEB_SENDING;
    bool ok = doPost(gPendingFen, gPendingSeq);
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

const String& webPublishGetBoardID() {
    return gBoardID;
}

String webPublishStatusPayload(const char *prefix) {
    String s = String(prefix);
    s += "|url=";
    s += gServerUrl.length() > 0 ? gServerUrl : "-";
    s += "|game=";
    s += gGameID.length() > 0 ? gGameID : "-";
    s += "|board=";
    s += gBoardID.length() > 0 ? gBoardID : "-";
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
