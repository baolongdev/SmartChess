# 02 — Hardware and Build

## Bill of Materials

| Component | Model | Notes |
|-----------|-------|-------|
| Microcontroller | ESP32-S3-DevKitC-1 | 16 MB flash, PSRAM enabled |
| RFID reader | MFRC522 | SPI interface, 3.3 V logic |
| Antenna coils | Custom PCB, 64 units | One per chess square |
| RFID tags | MIFARE Classic 1K | Attached to each chess piece |

## Wiring

### SPI Bus (MFRC522)

| MFRC522 pin | ESP32-S3 GPIO |
|-------------|--------------|
| SCK | 14 |
| MISO | 21 |
| MOSI | 2 |
| SDA (SS/CS) | 1 |
| RST | 48 |
| 3.3 V | 3V3 |
| GND | GND |

### Antenna Matrix (12 control pins)

The 64 antenna coils are multiplexed via 12 GPIO pins: six for odd-indexed squares and six for even-indexed squares.

| Group | GPIOs |
|-------|-------|
| ODD | 6, 15, 17, 8, 10, 12 |
| EVEN | 7, 16, 18, 9, 11, 13 |

The full 64-entry mapping (square → GPIO bitmask pair) is defined in `include/core/SmartChessConfig.h` as `ANTENNA_ARRAY`. Each entry encodes which combination of odd/even pins selects that particular coil.

## PlatformIO Configuration

Defined in `platformio.ini`:

```ini
[env:esp32s3]
platform              = espressif32
board                 = esp32-s3-devkitc-1
framework             = arduino

monitor_speed         = 115200
upload_speed          = 921600

board_build.flash_size = 16MB
board_build.psram      = enabled

lib_deps =
    miguelbalboa/MFRC522 @ ^1.4.12
```

## Project Directory Layout

```
SmartChess/
├── src/
│   ├── main.cpp
│   ├── app/
│   │   └── SmartChessApp.cpp
│   ├── chess/
│   │   ├── BoardState.cpp
│   │   ├── MoveGen.cpp
│   │   └── Fen.cpp
│   ├── hardware/
│   │   └── RfidScanner.cpp
│   ├── ble/
│   │   └── BleFen.cpp
│   ├── net/
│   │   ├── LichessPublish.cpp
│   │   ├── BoardRegistration.cpp
│   │   └── WebMovePublish.cpp
│   └── utils/
│       ├── ScanDebug.cpp
│       └── TextUtils.cpp
├── include/
│   ├── SmartChessConfig.h
│   ├── SmartChessTypes.h
│   ├── app/
│   ├── chess/
│   ├── hardware/
│   ├── ble/
│   ├── net/
│   └── utils/
├── web-client/
├── docs/
├── ble_fen_client.py
├── platformio.ini
└── README.md
```

## Build Commands

```bash
# Compile firmware
pio run

# Compile and upload to connected board
pio run -t upload

# Open serial monitor at 115200 baud
pio device monitor

# Clean build artifacts
pio run -t clean
```

## Resource Usage (approximate)

| Resource | Usage |
|----------|-------|
| Flash | ~28% of 16 MB |
| PSRAM | Enabled, used for heap allocations |
| RAM | Fits comfortably within ESP32-S3 SRAM |

## Related Documents

- [Firmware Workflow](03-firmware-workflow.md)
- [System Overview](01-overview.md)


