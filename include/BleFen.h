#pragma once

#include <Arduino.h>

using BleCommandHandler = bool (*)(const String &command, String &response);

void bleFenBegin();
void bleFenPublish(const String &fen);
bool bleFenIsConnected();
void bleFenPoll();
void bleFenSetCommandHandler(BleCommandHandler handler);
void bleFenLog(const String &line);
