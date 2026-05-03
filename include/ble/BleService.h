#pragma once

// ---------------------------------------------------------------------------
// BleService.h — BLE GATT service for SmartChess ESP32.
//
// Service UUID : 3f0e0001-70a1-4f8a-a6a3-51e9590e9f20
// FEN char     : 3f0e0002  READ  — static board FEN (not notified; game data goes via WiFi)
// CMD char     : 3f0e0003  READ | WRITE | NOTIFY  — config command channel
// LOG char     : 3f0e0004  READ | NOTIFY  — log messages / WiFi scan results
// OTA char     : 3f0e0005  WRITE | NOTIFY  — raw firmware binary for OTA
// ---------------------------------------------------------------------------

#include <Arduino.h>

/** Command handler: returns true on success, sets response string. */
using BleCommandHandler  = bool (*)(const String &command, String &response);

/** OTA data callback — invoked from the BLE OTA characteristic's onWrite. */
using BleOtaDataCallback = void (*)(const uint8_t *data, size_t len);

/** Initialize BLE server, create GATT service, start advertising. */
void bleServiceBegin();

/** Returns true if a BLE central is currently connected. */
bool bleIsConnected();

/**
 * Must be called once per loop().  Dispatches pending BLE command writes
 * to the registered handler and flushes log notifications.
 */
void bleServicePoll();

/** Register the callback that handles incoming BLE CMD writes. */
void bleSetCommandHandler(BleCommandHandler handler);

/** Queue a log line for notification on the LOG characteristic. */
void bleLog(const String &line);

/**
 * Send a log line immediately and synchronously on the LOG characteristic.
 * Safe to call from within a BLE command handler (before CMD ACK is sent).
 * Use for streaming multiple items (e.g. WiFi scan results) before the ACK.
 */
void bleLogImmediate(const String &line);

/** Register callback for OTA writes on the OTA characteristic (UUID 3f0e0005). */
void bleSetOtaCallback(BleOtaDataCallback cb);

/** Send a response notification on the OTA characteristic. */
void bleOtaRespond(const char *response);
