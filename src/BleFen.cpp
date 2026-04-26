#include "BleFen.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

namespace {

constexpr const char *kDeviceName = "SmartChess-ESP32S3";
constexpr const char *kServiceUuid = "3f0e0001-70a1-4f8a-a6a3-51e9590e9f20";
constexpr const char *kFenCharUuid = "3f0e0002-70a1-4f8a-a6a3-51e9590e9f20";
constexpr const char *kCmdCharUuid = "3f0e0003-70a1-4f8a-a6a3-51e9590e9f20";
constexpr const char *kLogCharUuid = "3f0e0004-70a1-4f8a-a6a3-51e9590e9f20";

constexpr const char *kBleHelp =
  "Use app: nRF Connect / LightBlue | Service: 3f0e0001-70a1-4f8a-a6a3-51e9590e9f20 | Char: 3f0e0002-70a1-4f8a-a6a3-51e9590e9f20";

BLEServer *gServer = nullptr;
BLECharacteristic *gFenCharacteristic = nullptr;
BLECharacteristic *gCmdCharacteristic = nullptr;
BLECharacteristic *gLogCharacteristic = nullptr;
bool gBleConnected = false;
BleCommandHandler gCommandHandler = nullptr;
String gLastCmd = "";
String gPendingCmd = "";
volatile bool gCmdReady = false;
String gPendingLog = "";
volatile bool gLogReady = false;

class CommandCallbacks : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic *characteristic) override {
    if (characteristic == nullptr) {
      return;
    }

    String raw = characteristic->getValue().c_str();
    raw.trim();
    if (raw.length() == 0) {
      return;
    }

    gPendingCmd = raw;
    gCmdReady = true;
  }
};

class ServerCallbacks : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer *server) override {
    (void)server;
    gBleConnected = true;
    Serial.println(F("[BLE] Connected"));
  }

  void onDisconnect(BLEServer *server) override {
    (void)server;
    gBleConnected = false;
    Serial.println(F("[BLE] Disconnected, advertising..."));
    BLEDevice::startAdvertising();
  }
};

}  // namespace

void bleFenBegin() {
  BLEDevice::init(kDeviceName);
  gServer = BLEDevice::createServer();
  gServer->setCallbacks(new ServerCallbacks());

  BLEService *service = gServer->createService(kServiceUuid);

  gFenCharacteristic = service->createCharacteristic(
    kFenCharUuid,
    BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_NOTIFY
  );
  gFenCharacteristic->addDescriptor(new BLE2902());
  gFenCharacteristic->setValue("-");

  gCmdCharacteristic = service->createCharacteristic(
    kCmdCharUuid,
    BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_NOTIFY
  );
  gCmdCharacteristic->addDescriptor(new BLE2902());
  gCmdCharacteristic->setCallbacks(new CommandCallbacks());
  gCmdCharacteristic->setValue("READY");

  gLogCharacteristic = service->createCharacteristic(
    kLogCharUuid,
    BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_NOTIFY
  );
  gLogCharacteristic->addDescriptor(new BLE2902());
  gLogCharacteristic->setValue("-");

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println(F("[BLE] FEN service started"));
  Serial.print(F("[BLE] Device: "));
  Serial.println(kDeviceName);
  Serial.print(F("[BLE] Service UUID: "));
  Serial.println(kServiceUuid);
  Serial.print(F("[BLE] Char UUID: "));
  Serial.println(kFenCharUuid);
  Serial.print(F("[BLE] Cmd UUID: "));
  Serial.println(kCmdCharUuid);
  Serial.print(F("[BLE] Log UUID: "));
  Serial.println(kLogCharUuid);
  Serial.println(kBleHelp);
}

void bleFenPublish(const String &fen) {
  if (gFenCharacteristic == nullptr) {
    return;
  }
  gFenCharacteristic->setValue(fen.c_str());
  if (gBleConnected) {
    gFenCharacteristic->notify();
  }
}

bool bleFenIsConnected() {
  return gBleConnected;
}

void bleFenPoll() {
  if (gCmdReady) {
    noInterrupts();
    gCmdReady = false;
    String cmd = gPendingCmd;
    gPendingCmd = "";
    interrupts();

    String response = "ERR: no handler";
    bool ok = false;
    if (gCommandHandler != nullptr) {
      ok = gCommandHandler(cmd, response);
    }

    gLastCmd = cmd;
    String ack = ok ? String("OK: ") + response : String("ERR: ") + response;
    if (gCmdCharacteristic != nullptr) {
      gCmdCharacteristic->setValue(ack.c_str());
      if (gBleConnected) {
        gCmdCharacteristic->notify();
      }
    }

    String logLine = String("[CMD] ") + cmd + String(" -> ") + response;
    Serial.println(logLine);
    if (gLogCharacteristic != nullptr) {
      gLogCharacteristic->setValue(logLine.c_str());
      if (gBleConnected) {
        gLogCharacteristic->notify();
      }
    }
  }

  if (gLogReady) {
    noInterrupts();
    gLogReady = false;
    String line = gPendingLog;
    gPendingLog = "";
    interrupts();

    if (gLogCharacteristic != nullptr) {
      gLogCharacteristic->setValue(line.c_str());
      if (gBleConnected) {
        gLogCharacteristic->notify();
      }
    }
  }
}

void bleFenSetCommandHandler(BleCommandHandler handler) {
  gCommandHandler = handler;
}

void bleFenLog(const String &line) {
  noInterrupts();
  gPendingLog = line;
  gLogReady = true;
  interrupts();
}
