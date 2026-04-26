#include "ScanDebug.h"

#include "BoardState.h"

static void printMsFromUs(uint32_t us) {
  Serial.print(us / 1000);
  Serial.print('.');
  uint32_t frac = us % 1000;
  if (frac < 100) {
    Serial.print('0');
  }
  if (frac < 10) {
    Serial.print('0');
  }
  Serial.print(frac);
}

void printScanSnapshot(const String inBuf[NUM_ANTENNAS], const __FlashStringHelper *title) {
  Serial.println();
  Serial.println(title);
  Serial.println(F("  +---+---+---+---+---+---+---+---+"));
  for (int rank = 7; rank >= 0; rank--) {
    Serial.print(rank + 1);
    Serial.print(F(" |"));
    for (int file = 0; file < 8; file++) {
      int idx = squareIdx(file, rank);
      Serial.print(inBuf[idx].length() > 0 ? F(" 1 ") : F("   "));
      Serial.print('|');
    }
    Serial.println();
    Serial.println(F("  +---+---+---+---+---+---+---+---+"));
  }
  Serial.println(F("    a   b   c   d   e   f   g   h"));

  Serial.println(F("[SCAN_UID]"));
  bool any = false;
  for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
    if (inBuf[idx].length() == 0) {
      continue;
    }
    any = true;
    Serial.print(squareNameFromIdx(idx));
    Serial.print('=');
    Serial.print(inBuf[idx]);
    Serial.print(' ');
  }
  if (!any) {
    Serial.print(F("(khong co tag)"));
  }
  Serial.println();
}

void printScanCompact(const String inBuf[NUM_ANTENNAS],
                      bool scanVerbose,
                      const char board[8][8],
                      bool usePieceLetters) {
  int count = 0;
  for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
    if (inBuf[idx].length() > 0) {
      count++;
    }
  }

  Serial.print(F("[SCAN_BOARD] tags="));
  Serial.println(count);

  Serial.println(F("  +---+---+---+---+---+---+---+---+"));
  for (int rank = 7; rank >= 0; rank--) {
    Serial.print(rank + 1);
    Serial.print(F(" |"));
    for (int file = 0; file < 8; file++) {
      int idx = squareIdx(file, rank);
      if (inBuf[idx].length() == 0) {
        Serial.print(F(" . "));
      } else if (usePieceLetters) {
        char p = board[file][rank];
        if (p == '.') {
          Serial.print(F(" ? "));
        } else {
          Serial.print(' ');
          Serial.print(p);
          Serial.print(' ');
        }
      } else {
        Serial.print(F(" 1 "));
      }
      Serial.print('|');
    }
    Serial.println();
    Serial.println(F("  +---+---+---+---+---+---+---+---+"));
  }
  Serial.println(F("    a   b   c   d   e   f   g   h"));

  if (scanVerbose) {
    Serial.print(F("[SCAN_UID] "));
    bool any = false;
    for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
      if (inBuf[idx].length() == 0) {
        continue;
      }
      any = true;
      Serial.print(squareNameFromIdx(idx));
      Serial.print('=');
      Serial.print(inBuf[idx]);
      Serial.print(' ');
    }
    if (!any) {
      Serial.print(F("(khong co tag)"));
    }
    Serial.println();
  }
}

void printScanTiming(uint16_t scannedCount,
                     uint32_t lastFullScanUs,
                     uint32_t lastAvgCellUs,
                     uint32_t lastMinCellUs,
                     uint32_t lastMaxCellUs) {
  Serial.print(F("[TIME] 1_o: avg="));
  Serial.print(lastAvgCellUs);
  Serial.print(F("us min="));
  Serial.print(lastMinCellUs);
  Serial.print(F("us max="));
  Serial.print(lastMaxCellUs);
  Serial.print(F("us | "));
  Serial.print(scannedCount);
  Serial.print(F("_o: "));
  printMsFromUs(lastFullScanUs);
  Serial.print(F("ms"));

  if (lastFullScanUs > 0) {
    uint32_t hzX100 = 100000000UL / lastFullScanUs;
    Serial.print(F(" (~"));
    Serial.print(hzX100 / 100);
    Serial.print('.');
    uint32_t hzFrac = hzX100 % 100;
    if (hzFrac < 10) {
      Serial.print('0');
    }
    Serial.print(hzFrac);
    Serial.print(F(" Hz)"));
  }
  Serial.println();
}
