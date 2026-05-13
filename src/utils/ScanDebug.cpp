#include "utils/ScanDebug.h"

#include "chess/BoardState.h"

void printScanSnapshot(const String inBuf[NUM_ANTENNAS], const __FlashStringHelper *title) {
  String out;
  out.reserve(512);
  out += '\n'; out += title; out += '\n';
  out += F("  +---+---+---+---+---+---+---+---+\n");
  for (int rank = 7; rank >= 0; rank--) {
    out += (char)('1' + rank);
    out += F(" |");
    for (int file = 0; file < 8; file++) {
      int idx = squareIdx(file, rank);
      out += (inBuf[idx].length() > 0) ? F(" 1 ") : F("   ");
      out += '|';
    }
    out += '\n';
    out += F("  +---+---+---+---+---+---+---+---+\n");
  }
  out += F("    a   b   c   d   e   f   g   h\n");

  out += F("[SCAN_UID] ");
  bool any = false;
  for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
    if (inBuf[idx].length() == 0) continue;
    any = true;
    out += squareNameFromIdx(idx);
    out += '=';
    out += inBuf[idx];
    out += ' ';
  }
  if (!any) out += F("(khong co tag)");
  out += '\n';
  Serial.print(out);
}

void printScanCompact(const String inBuf[NUM_ANTENNAS],
                      bool scanVerbose,
                      const char board[8][8],
                      bool usePieceLetters) {
  int count = 0;
  for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
    if (inBuf[idx].length() > 0) count++;
  }

  String out;
  out.reserve(640);
  out += F("[SCAN_BOARD] tags=");
  out += count;
  out += '\n';

  out += F("  +---+---+---+---+---+---+---+---+\n");
  for (int rank = 7; rank >= 0; rank--) {
    out += (char)('1' + rank);
    out += " |";
    for (int file = 0; file < 8; file++) {
      int idx = squareIdx(file, rank);
      if (inBuf[idx].length() == 0) {
        out += " . ";
      } else if (usePieceLetters) {
        char p = board[file][rank];
        if (p == '.') {
          out += " ? ";
        } else {
          out += ' ';
          out += p;
          out += ' ';
        }
      } else {
        out += " 1 ";
      }
      out += '|';
    }
    out += '\n';
    out += "  +---+---+---+---+---+---+---+---+\n";
  }
  out += "    a   b   c   d   e   f   g   h\n";

  if (scanVerbose) {
    out += "[SCAN_UID] ";
    bool any = false;
    for (int idx = 0; idx < NUM_ANTENNAS; idx++) {
      if (inBuf[idx].length() == 0) continue;
      any = true;
      out += squareNameFromIdx(idx);
      out += '=';
      out += inBuf[idx];
      out += ' ';
    }
    if (!any) out += "(khong co tag)";
    out += '\n';
  }
  Serial.print(out);
}

void printScanTiming(uint16_t scannedCount,
                     uint32_t lastFullScanUs,
                     uint32_t lastAvgCellUs,
                     uint32_t lastMinCellUs,
                     uint32_t lastMaxCellUs) {
  String out;
  out.reserve(160);
  out += "[TIME] 1_o: avg=";
  out += lastAvgCellUs;
  out += "us min=";
  out += lastMinCellUs;
  out += "us max=";
  out += lastMaxCellUs;
  out += "us | ";
  out += scannedCount;
  out += "_o: ";
  out += lastFullScanUs / 1000;
  out += '.';
  uint32_t frac = lastFullScanUs % 1000;
  if (frac < 100) out += '0';
  if (frac < 10) out += '0';
  out += frac;
  out += "ms";

  if (lastFullScanUs > 0) {
    uint32_t hzX100 = 100000000UL / lastFullScanUs;
    out += " (~";
    out += hzX100 / 100;
    out += '.';
    uint32_t hzFrac = hzX100 % 100;
    if (hzFrac < 10) out += '0';
    out += hzFrac;
    out += " Hz)";
  }
  out += '\n';
  Serial.print(out);
}
