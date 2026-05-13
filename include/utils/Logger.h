#pragma once

#include <Arduino.h>

enum LogLevel {
  LOG_LEVEL_ERROR = 0,
  LOG_LEVEL_WARN  = 1,
  LOG_LEVEL_INFO  = 2,
  LOG_LEVEL_DEBUG = 3,
};

void logInit(LogLevel level = LOG_LEVEL_DEBUG);
void logSetLevel(LogLevel level);
void logSetSerial2(HardwareSerial *port);

void logE(const char *tag, const String &msg);
void logW(const char *tag, const String &msg);
void logI(const char *tag, const String &msg);
void logD(const char *tag, const String &msg);
