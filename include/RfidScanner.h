#pragma once

#include <Arduino.h>
#include <MFRC522.h>
#include "SmartChessConfig.h"

void initRfidScanner(MFRC522 &mfrc522);
String scanUID(MFRC522 &mfrc522, int idx);
void selectiveScanToBuffer(MFRC522 &mfrc522,
                           const bool mask[NUM_ANTENNAS],
                           String outBuf[NUM_ANTENNAS],
                           uint16_t &scannedCount,
                           uint32_t &lastFullScanUs,
                           uint32_t &lastAvgCellUs,
                           uint32_t &lastMinCellUs,
                           uint32_t &lastMaxCellUs);
void fullScanToBuffer(MFRC522 &mfrc522,
                      String outBuf[NUM_ANTENNAS],
                      uint16_t &scannedCount,
                      uint32_t &lastFullScanUs,
                      uint32_t &lastAvgCellUs,
                      uint32_t &lastMinCellUs,
                      uint32_t &lastMaxCellUs);
