#pragma once

#include <Arduino.h>

String settingsFieldValue(const String &payload, const String &keyUpper);
bool parseBoolToken(const String &token, bool &value);
String urlEncode(const String &src);
