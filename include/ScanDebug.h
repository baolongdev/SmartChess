#pragma once

#include <Arduino.h>
#include "SmartChessConfig.h"

void printScanSnapshot(const String inBuf[NUM_ANTENNAS], const __FlashStringHelper *title);
void printScanCompact(const String inBuf[NUM_ANTENNAS],
                      bool scanVerbose,
                      const char board[8][8],
                      bool usePieceLetters);
void printScanTiming(uint16_t scannedCount,
                     uint32_t lastFullScanUs,
                     uint32_t lastAvgCellUs,
                     uint32_t lastMinCellUs,
                     uint32_t lastMaxCellUs);
