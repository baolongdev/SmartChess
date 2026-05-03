#pragma once

// ---------------------------------------------------------------------------
// WebPublish.h — HTTP POST moves to BEChessWeb server after each confirmed
// move.  Non-blocking: the actual POST happens inside webPublishPoll().
//
// BLE command interface (handled in SmartChessApp via handleCommonCommand):
//   WEB                                          → print current config
//   WEB_SET|URL=http://...:8080|GAME=game_alpha|ENABLED=1
// ---------------------------------------------------------------------------

#include <Arduino.h>

/** Initialize with server URL, game ID, and enabled flag. */
void webPublishBegin(const String &serverUrl, const String &gameID, bool enabled);

/**
 * Queue a move for HTTP delivery.  Non-blocking — actual POST happens in
 * webPublishPoll().
 * @param uci  UCI string, e.g. "e2e4" or "e7e8q" (promotion)
 * @param seq  1-based move sequence number (used by server for dedup)
 */
void webPublishMove(const String &uci, int seq);

/** Process one pending HTTP job.  Call once per loop() iteration. */
void webPublishPoll();

/** Update config at runtime (e.g. after WEB_SET BLE command). */
void webPublishSetConfig(const String &serverUrl, const String &gameID, bool enabled);

bool          webPublishIsEnabled();
const String& webPublishGetServerUrl();
const String& webPublishGetGameID();

/**
 * HTTP status code of the most recent POST attempt.
 * 0   = no POST attempted yet (or move just queued, gLastStatus reset).
 * 2xx = server accepted.
 * 4xx = server rejected (illegal move, etc.) — caller should rollback seq.
 * <0  = network-level error (no HTTP response received).
 */
int webPublishGetLastStatus();

/** Human-readable status payload for WEB command response. */
String webPublishStatusPayload(const char *prefix);
