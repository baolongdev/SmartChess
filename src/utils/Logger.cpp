#include "utils/Logger.h"
#include "ble/BleService.h"

static LogLevel gLevel = LOG_LEVEL_DEBUG;
static HardwareSerial *gSerial2 = nullptr;

void logInit(LogLevel level) { gLevel = level; }
void logSetLevel(LogLevel level) { gLevel = level; }
void logSetSerial2(HardwareSerial *port) { gSerial2 = port; }

static void logOut(const char *tag, const char *level, const char *ansi, const String &msg) {
  char buf[288];
  unsigned long now = millis();
  int hdr = snprintf(buf, sizeof(buf) - 3, "[%6lu] [%s] [%-5s] ", now, level, tag);
  if (hdr < 0) return;
  int cpy = msg.length();
  if (cpy > (int)sizeof(buf) - hdr - 2) cpy = sizeof(buf) - hdr - 2;
  memcpy(buf + hdr, msg.c_str(), cpy);
  buf[hdr + cpy] = '\n';
  buf[hdr + cpy + 1] = '\0';

  if (ansi) Serial.print(ansi);
  Serial.print(buf);
  if (ansi) Serial.print(F("\033[0m"));
  if (gSerial2) gSerial2->print(buf);

  bleLog(String(buf));
}

void logE(const char *tag, const String &msg) {
  if (gLevel < LOG_LEVEL_ERROR) return;
  logOut(tag, "ERR", "\033[31m", msg);
}

void logW(const char *tag, const String &msg) {
  if (gLevel < LOG_LEVEL_WARN) return;
  logOut(tag, "WRN", "\033[33m", msg);
}

void logI(const char *tag, const String &msg) {
  if (gLevel < LOG_LEVEL_INFO) return;
  logOut(tag, "INF", "\033[32m", msg);
}

void logD(const char *tag, const String &msg) {
  if (gLevel < LOG_LEVEL_DEBUG) return;
  logOut(tag, "DBG", "\033[2m", msg);
}
