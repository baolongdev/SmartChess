#include "utils/TextUtils.h"

String settingsFieldValue(const String &payload, const String &keyUpper) {
  String payloadUpper = payload;
  payloadUpper.toUpperCase();

  String marker = keyUpper + String("=");
  int start = payloadUpper.indexOf(marker);
  if (start < 0) {
    return "";
  }

  start += marker.length();
  int end = payloadUpper.indexOf('|', start);
  if (end < 0) {
    end = payload.length();
  }
  return payload.substring(start, end);
}

bool parseBoolToken(const String &token, bool &value) {
  String t = token;
  t.trim();
  t.toUpperCase();

  if (t == "1" || t == "TRUE" || t == "ON" || t == "YES") {
    value = true;
    return true;
  }
  if (t == "0" || t == "FALSE" || t == "OFF" || t == "NO") {
    value = false;
    return true;
  }
  return false;
}

String urlEncode(const String &src) {
  const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(src.length() * 3);

  for (size_t i = 0; i < src.length(); i++) {
    uint8_t c = (uint8_t)src[i];
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }

  return out;
}

