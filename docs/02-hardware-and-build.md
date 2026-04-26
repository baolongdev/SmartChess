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

The full 64-entry mapping (square → GPIO bitmask pair) is defined in `include/SmartChessConfig.h` as `ANTENNA_ARRAY`. Each entry encodes which combination of odd/even pins selects that particular coil.

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
│   ├── main.cpp              # setup() / loop() entry point
│   ├── SmartChessApp.cpp     # Game state machine, move tracking, command handling
│   ├── RfidScanner.cpp       # Antenna multiplexing and RFID UID reading
│   ├── MoveGen.cpp           # Candidate move generation per piece type
│   ├── Fen.cpp               # 6-field FEN string construction
│   ├── BoardState.cpp        # Board indexing and piece placement utilities
│   ├── BleFen.cpp            # BLE GATT server and characteristic callbacks
│   └── ScanDebug.cpp         # Board dump and scan timing diagnostics
├── include/
│   ├── SmartChessConfig.h    # Pin definitions, ANTENNA_ARRAY, timing constants
│   ├── SmartChessTypes.h     # Enums (ScanState, MoveKind, PieceType), structs
│   ├── SmartChessApp.h       # Public API: smartChessBegin() / smartChessTick()
│   ├── RfidScanner.h
│   ├── BleFen.h
│   ├── Fen.h
│   ├── MoveGen.h
│   └── BoardState.h
├── web-client/
│   ├── index.html
│   ├── app.js
│   └── styles.css
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
