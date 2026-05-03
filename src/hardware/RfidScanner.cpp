#include "hardware/RfidScanner.h"

#include <SPI.h>
#include <driver/gpio.h>
#include <soc/gpio_reg.h>

static void setupRC522Timeout(MFRC522 &mfrc522) {
  mfrc522.PCD_WriteRegister(mfrc522.TModeReg, 0x80);
  mfrc522.PCD_WriteRegister(mfrc522.TPrescalerReg, 0xA9);
  mfrc522.PCD_WriteRegister(mfrc522.TReloadRegH, 0x00);
  mfrc522.PCD_WriteRegister(mfrc522.TReloadRegL, 0x78);
}

static void set12Pins(uint8_t value) {
  uint32_t setMask = 0;
  uint32_t clrMask = 0;
  for (int i = 0; i < 6; i++) {
    if ((value >> i) & 0x01) {
      setMask |= (1UL << EVEN_PINS[i]);
      clrMask |= (1UL << ODD_PINS[i]);
    } else {
      clrMask |= (1UL << EVEN_PINS[i]);
      setMask |= (1UL << ODD_PINS[i]);
    }
  }
  REG_WRITE(GPIO_OUT_W1TC_REG, clrMask);
  REG_WRITE(GPIO_OUT_W1TS_REG, setMask);
}

void initRfidScanner(MFRC522 &mfrc522) {
  SPI.end();
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  mfrc522.PCD_Init();
  setupRC522Timeout(mfrc522);

  for (int i = 0; i < 6; i++) {
    gpio_reset_pin((gpio_num_t)EVEN_PINS[i]);
    gpio_reset_pin((gpio_num_t)ODD_PINS[i]);
    pinMode(EVEN_PINS[i], OUTPUT);
    pinMode(ODD_PINS[i], OUTPUT);
  }
}

String scanUID(MFRC522 &mfrc522, int idx) {
  set12Pins(ANTENNA_ARRAY[idx]);
  delayMicroseconds(400);

  byte atqa[2];
  byte atqaSize = sizeof(atqa);
  MFRC522::StatusCode wakeStatus = mfrc522.PICC_WakeupA(atqa, &atqaSize);
  if ((wakeStatus == MFRC522::STATUS_OK || wakeStatus == MFRC522::STATUS_COLLISION) &&
      mfrc522.PICC_ReadCardSerial()) {
    String uid = "";
    for (byte b = 0; b < mfrc522.uid.size; b++) {
      if (mfrc522.uid.uidByte[b] < 0x10) {
        uid += "0";
      }
      uid += String(mfrc522.uid.uidByte[b], HEX);
    }
    uid.toUpperCase();
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    return uid;
  }
  return "";
}

// Fast presence-only check — skips ReadCardSerial, ~6× faster than scanUID.
// Returns true if any card responds to WUPA. Use during SCAN_IDLE tracking
// where only lift/land detection is needed (UID already known from initial scan).
bool scanPresent(MFRC522 &mfrc522, int idx) {
  set12Pins(ANTENNA_ARRAY[idx]);
  delayMicroseconds(200);   // shorter settle — presence check is less demanding

  byte atqa[2];
  byte atqaSize = sizeof(atqa);
  MFRC522::StatusCode status = mfrc522.PICC_WakeupA(atqa, &atqaSize);
  bool present = (status == MFRC522::STATUS_OK || status == MFRC522::STATUS_COLLISION);
  if (present) {
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
  return present;
}

void selectiveScanToBuffer(MFRC522 &mfrc522,
                           const bool mask[NUM_ANTENNAS],
                           String outBuf[NUM_ANTENNAS],
                           uint16_t &scannedCount,
                           uint32_t &lastFullScanUs,
                           uint32_t &lastAvgCellUs,
                           uint32_t &lastMinCellUs,
                           uint32_t &lastMaxCellUs) {
  uint32_t totalStartUs = micros();
  uint32_t sumCellUs = 0;
  uint32_t minCellUs = 0xFFFFFFFF;
  uint32_t maxCellUs = 0;
  scannedCount = 0;

  for (int i = 0; i < NUM_ANTENNAS; i++) {
    if (mask != nullptr && !mask[i]) {
      continue;
    }

    uint32_t cellStartUs = micros();
    outBuf[i] = scanUID(mfrc522, i);
    uint32_t cellUs = micros() - cellStartUs;
    sumCellUs += cellUs;
    scannedCount++;

    if (cellUs < minCellUs) {
      minCellUs = cellUs;
    }
    if (cellUs > maxCellUs) {
      maxCellUs = cellUs;
    }
  }

  lastFullScanUs = micros() - totalStartUs;
  if (scannedCount == 0) {
    lastAvgCellUs = 0;
    lastMinCellUs = 0;
    lastMaxCellUs = 0;
  } else {
    lastAvgCellUs = sumCellUs / scannedCount;
    lastMinCellUs = minCellUs;
    lastMaxCellUs = maxCellUs;
  }
}

void fullScanToBuffer(MFRC522 &mfrc522,
                      String outBuf[NUM_ANTENNAS],
                      uint16_t &scannedCount,
                      uint32_t &lastFullScanUs,
                      uint32_t &lastAvgCellUs,
                      uint32_t &lastMinCellUs,
                      uint32_t &lastMaxCellUs) {
  selectiveScanToBuffer(mfrc522,
                        nullptr,
                        outBuf,
                        scannedCount,
                        lastFullScanUs,
                        lastAvgCellUs,
                        lastMinCellUs,
                        lastMaxCellUs);
}

