#pragma once

#include <Arduino.h>

void webPublishBegin(const String &serverUrl, const String &gameID, const String &boardID, bool enabled);

void webPublishSetConfig(const String &serverUrl, const String &gameID, const String &boardID, bool enabled);

void webPublishFEN(const String &fen, int seq);

void webPublishPoll();

bool          webPublishIsEnabled();
const String& webPublishGetServerUrl();
const String& webPublishGetGameID();
const String& webPublishGetBoardID();

int webPublishGetLastStatus();

String webPublishStatusPayload(const char *prefix);
