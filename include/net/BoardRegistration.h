#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// BoardRegistration — Periodic HTTP heartbeats + WiFi command delivery.
//
// Heartbeat:   POST <serverUrl>/boards/heartbeat  every NORMAL_MS (30 s)
//              or every FAST_MS (5 s) when game is not started.
// Body:        { "boardID", "gameID", "ip"  [, "scanEvent", "scanDetail"] }
// Response:    { "ok": true [, "gameID": "..." ] [, "command": "START" ] }
//
// Scan result: POST <serverUrl>/boards/scan-result  (immediate, one-shot)
// Body:        { "boardID", "gameID", "result": "STARTED"|"MISSING"|"DUPLICATE",
//               "detail": "" | "a1,b2,..." }
// ---------------------------------------------------------------------------

using BoardRegGameIDCallback  = void (*)(const String &newGameID);
using BoardRegCommandCallback = void (*)(const String &command);

/** Initialise. gLastSentMs=0 → first heartbeat fires on next tick. */
void boardRegBegin(const String &serverUrl,
                   const String &boardID,
                   const String &gameID = "");

/** Update active gameID (called when server assigns one). */
void boardRegSetGameID(const String &gameID);

/** Update server URL and boardID at runtime (after WEB_SET). */
void boardRegSetConfig(const String &serverUrl, const String &boardID);

/** Callback fired when server assigns a new gameID. */
void boardRegSetGameIDCallback(BoardRegGameIDCallback cb);

/** Callback fired when server sends a command in heartbeat response. */
void boardRegSetCommandCallback(BoardRegCommandCallback cb);

/**
 * Non-blocking poll — call once per loop().
 * Uses fast interval (5 s) when fastPoll=true, normal interval (30 s) otherwise.
 */
void boardRegTick();

/** Reset timer so next boardRegTick() fires immediately. */
void boardRegForceHeartbeat();

/** Switch between fast (5 s) and normal (30 s) heartbeat intervals. */
void boardRegSetFastPoll(bool fast);

/**
 * Queue a one-shot scan result POST to /boards/scan-result.
 * Sent on the very next boardRegTick() call, then cleared.
 * @param result  "STARTED" | "MISSING" | "DUPLICATE"
 * @param detail  "" when STARTED, comma-separated squares when MISSING/DUPLICATE
 */
void boardRegQueueScanResult(const String &result, const String &detail);

/**
 * Queue a one-shot in-game alert POST to /boards/alert.
 * Sent on the very next boardRegTick() call, then cleared.
 * @param code    Alert code — "WRONG_TURN" | "PIECE_LOST" | "ILLEGAL_DEST" |
 *                             "TRACK_TIMEOUT" | "FALLBACK_TIMEOUT" | "NO_LEGAL_DEST" | ...
 * @param detail  Human-readable context string (square names, UIDs, etc.)
 */
void boardRegQueueAlert(const String &code, const String &detail);

/** Mark the physical button as pressed since last heartbeat.
 *  The next heartbeat will include "btn":1, then the flag auto-clears. */
void boardRegSetBtnPressed();
