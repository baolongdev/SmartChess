# SmartChess

> A physical chess board that tracks piece movements in real-time using RFID, broadcasts position via Bluetooth Low Energy, and visualizes the game on a web interface.

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)](https://platformio.org)
[![Framework](https://img.shields.io/badge/Framework-Arduino-blue)](https://arduino.cc)
[![BLE](https://img.shields.io/badge/Wireless-BLE%205.0-brightgreen)](https://www.bluetooth.com)
[![License](https://img.shields.io/badge/License-MIT-lightgrey)](LICENSE)

---

## Overview

SmartChess is an embedded system that turns a standard physical chess board into a smart, connected device. Each piece carries an RFID tag; a 64-antenna matrix driven by an ESP32-S3 reads their positions. A state-machine firmware tracks every move, enforces turn order, generates standard FEN strings, and streams live game state to connected clients over BLE.

**Key capabilities:**

- Real-time piece detection across all 64 squares via RFID antenna matrix
- Move tracking with candidate-list filtering — only relevant squares are scanned
- Standard 6-field FEN generation (placement, active color, castling, en-passant, clocks)
- BLE GATT server with three characteristics: FEN feed, command channel, live log stream
- Turn enforcement — wrong-turn attempts are rejected at lift time
- False-lift debouncing and multi-round move verification
- Web client (Web Bluetooth API) with live board visualization
- Python CLI client for headless testing and debugging

---

## Table of Contents

- [Hardware](#hardware)
- [Project Structure](#project-structure)
- [Quick Start](#quick-start)
- [BLE Protocol](#ble-protocol)
- [Firmware Architecture](#firmware-architecture)
- [Web Client](#web-client)
- [Python Client](#python-client)
- [Documentation](#documentation)
- [Known Limitations](#known-limitations)

---

## Hardware

| Component | Model / Spec |
|-----------|-------------|
| Microcontroller | ESP32-S3 (`esp32-s3-devkitc-1`) |
| RFID Reader | MFRC522 (SPI bus) |
| Antenna Matrix | 64 custom antenna coils, 12-pin multiplexed |
| Flash | 16 MB |
| Wireless | BLE 5.0 (built-in) |

### Pin Assignment

**SPI (MFRC522):**

| Signal | GPIO |
|--------|------|
| SCK | 14 |
| MISO | 21 |
| MOSI | 2 |
| SS (CS) | 1 |
| RST | 48 |

**Antenna Matrix (12 control pins):**

| Group | GPIOs |
|-------|-------|
| ODD | 6, 15, 17, 8, 10, 12 |
| EVEN | 7, 16, 18, 9, 11, 13 |

The full 64-square mapping is defined in `include/SmartChessConfig.h` as `ANTENNA_ARRAY`.

---

## Project Structure

```
SmartChess/
├── src/
│   ├── main.cpp              # Entry point: setup() / loop()
│   ├── SmartChessApp.cpp     # Game state machine, move tracking, command handler
│   ├── RfidScanner.cpp       # Antenna multiplexing and UID scanning
│   ├── MoveGen.cpp           # Legal candidate move generation per piece type
│   ├── Fen.cpp               # 6-field FEN string construction
│   ├── BoardState.cpp        # Board indexing and piece placement utilities
│   ├── BleFen.cpp            # BLE GATT server, characteristics, callbacks
│   └── ScanDebug.cpp         # Board dump and scan timing diagnostics
├── include/
│   ├── SmartChessConfig.h    # Pin definitions, antenna array, timing constants
│   ├── SmartChessTypes.h     # Enums (ScanState, MoveKind, PieceType), structs
│   ├── SmartChessApp.h       # Public API: smartChessBegin() / smartChessTick()
│   ├── RfidScanner.h
│   ├── BleFen.h
│   ├── Fen.h
│   ├── MoveGen.h
│   └── BoardState.h
├── web-client/
│   ├── index.html            # Board UI, controls, FEN display, log panel
│   ├── app.js                # Web Bluetooth API, move rendering, ACK handling
│   └── styles.css            # Terminal-style dark theme
├── docs/                     # Detailed technical documentation
├── ble_fen_client.py         # Python BLE client (asyncio + bleak)
├── platformio.ini            # Build / upload / monitor configuration
└── README.md
```

---

## Quick Start

### Prerequisites

- [PlatformIO](https://platformio.org/install) (CLI or VS Code extension)
- Chrome or Edge (desktop) for the web client — Web Bluetooth is not supported in Firefox
- Python 3.8+ with `bleak` (`pip install bleak`) for the Python client

### 1. Build and Flash

```bash
# Build firmware
pio run

# Upload to board
pio run -t upload

# Open serial monitor (115200 baud)
pio device monitor
```

### 2. Start a Game

1. Place all 32 pieces in the standard starting position.
2. Connect to **SmartChess-ESP32S3** from the web client or Python client.
3. Send the `START` command — the firmware scans the 32 starting squares, validates all UIDs, and begins tracking.
4. Move pieces normally. The board state and FEN update automatically after each confirmed move.

### 3. Run the Web Client

```bash
python -m http.server 8080
```

Open `http://localhost:8080/web-client/` in Chrome or Edge.

---

## BLE Protocol

**Device name:** `SmartChess-ESP32S3`  
**Service UUID:** `3f0e0001-70a1-4f8a-a6a3-51e9590e9f20`

### Characteristics

| Name | UUID suffix | Properties | Description |
|------|-------------|------------|-------------|
| FEN | `...0002` | READ, NOTIFY | Current board position as 6-field FEN |
| CMD | `...0003` | READ, WRITE, NOTIFY | Command input / ACK output |
| LOG | `...0004` | READ, NOTIFY | Real-time runtime log stream |

### Commands and ACK Format

All CMD responses follow a strict `OK:` / `ERR:` prefix:

| Command | Success ACK | Error ACK |
|---------|-------------|-----------|
| `START` | `OK: STARTED` | `ERR: START_FAILED:MISSING_PIECES:e2,f7` |
| `STOP` | `OK: STOPPED` | — |
| `STATUS` | `OK: RUNNING` or `OK: IDLE` | — |
| `F` / `FEN` | `OK: <fen-string>` | `ERR: NO_GAME` |
| `CFG_GET` | `OK: CFG|ver=...|verbose=...|continuous=...|letters=...` | — |
| `CFG_SET|VER=..|VERBOSE=..|CONTINUOUS=..|LETTERS=..` | `OK: CFG_SAVED|...` | `ERR: CFG_ERR:...` |
| `WIFI_GET` | `OK: WIFI|ver=...|ssid=...|status=...|ip=...` | — |
| `WIFI_SET|SSID=...|PASS=...|AUTO=1|CONNECT=1` | `OK: WIFI_SAVED|...` | `ERR: WIFI_ERR:...` |
| `WIFI_SCAN` | `OK: WIFI_SCAN|count=n|ssid0=...` | `ERR: WIFI_ERR:SCAN` |
| `WIFI_CONNECT` / `WIFI_DISCONNECT` | `OK: WIFI_CONNECTING|...` / `OK: WIFI_DISCONNECTED|...` | `ERR: WIFI_ERR:...` |
| `LICHESS_PUBLISH` | `OK: LICHESS_QUEUED|state=QUEUED|ply=n` | `ERR: LICHESS_ERR:...` |
| `LICHESS_STATUS` | `OK: LICHESS|state=...|url=...|err=...` | — |
| `LICHESS_STREAM_ON` / `LICHESS_STREAM_OFF` | `OK: LICHESS_STREAM_ON|...` / `OK: LICHESS_STREAM_OFF|...` | — |
| *(other)* | — | `ERR: UNKNOWN_CMD` |

When Wi-Fi is connected, Lichess publish is streamed automatically after each confirmed move (default ON), and the latest full movelist is re-pushed if new moves arrive during an active upload.

### FEN Keepalive

The firmware republishes the current FEN every **~1.5 s** while a BLE client is connected, ensuring clients can recover from missed notifications.

---

## Firmware Architecture

### State Machine

The scan loop runs a six-state machine on every `smartChessTick()` call:

```
SCAN_IDLE
  │  occupied square loses UID
  ▼
SCAN_LIFT_PENDING  ──(glitch / UID returns)──► SCAN_IDLE
  │  debounce confirmed
  ▼
SCAN_PIECE_LIFTED  ──(wrong turn)──► SCAN_FALLBACK
  │  generate candidate list
  ▼
SCAN_TRACKING_DESTINATION
  │  UID found in candidates        │  timeout
  ▼                                 ▼
SCAN_VERIFY                    SCAN_FALLBACK
  │  N stable rounds                │  timeout
  ▼                                 ▼
apply move, broadcast FEN      abandon, prompt re-place
```

### Key Timing Constants

| Constant | Default | Purpose |
|----------|---------|---------|
| `DEBOUNCE_LIFT_MS` | 50 ms | Minimum duration before a lift is confirmed |
| `TRACKING_TIMEOUT_MS` | 3 000 ms | Window to find the destination square |
| `FALLBACK_TIMEOUT_MS` | 5 000 ms | Total recovery budget before abandoning |
| `VERIFY_ROUNDS` | 2 | Stable scan rounds required to confirm a move |
| `BLE_KEEPALIVE_MS` | 1 500 ms | FEN re-broadcast interval |
| `CMD_COMMIT_IDLE_MS` | 120 ms | Serial auto-commit timeout (no Enter needed) |

### Scan Performance

| Operation | Typical Duration |
|-----------|-----------------|
| Single antenna | 300 – 500 µs |
| Full 64-square scan | 30 – 40 ms |
| Selective scan (subset) | proportional |

### Move Generation

Candidate destination squares are generated per piece type according to standard chess movement rules:

| Piece | Movement |
|-------|----------|
| Pawn | Forward 1, capture diagonals, en-passant, auto-promote to Queen |
| Knight | 8 L-shaped offsets |
| Bishop | Sliding diagonals |
| Rook | Sliding ranks and files |
| Queen | All sliding directions |
| King | Single-step in any direction |

> **Note:** Check, pin, and checkmate are not validated in firmware. See [Known Limitations](#known-limitations).

### Game Lifecycle

```
Power on
  └─► smartChessBegin()  — init BLE, RFID, idle state

Receive START
  └─► scan 32 starting squares
       └─► validate: 32 UIDs present, no duplicates
            ├─► fail → ERR: START_FAILED:...
            └─► pass → map UID→piece, enable state machine, ACK OK: STARTED

Active game
  └─► smartChessTick() every loop iteration
       ├─► state machine advances
       ├─► confirmed move → applyMove() → update FEN → bleFenPublish()
       └─► keepalive timer → bleFenPublish()

Receive STOP
  └─► disable state machine, reset game state, ACK OK: STOPPED
```

---

## Web Client

Located in `web-client/`. A single-page app that connects via the Web Bluetooth API.

**Features:**
- Live 8×8 board rendered from the current FEN
- Highlighted last move (source and destination squares)
- Real-time log panel showing BLE LOG notifications
- START / STOP / STATUS / FEN buttons
- Automatic UI reset on START failure, STOP, or BLE disconnect

**Supported browsers:** Chrome 56+, Edge 79+ (desktop only).

**Run locally:**
```bash
python -m http.server 8080
# open http://localhost:8080/web-client/
```

---

## Python Client

`ble_fen_client.py` is a command-line BLE client for testing without a browser.

**Requirements:**
```bash
pip install bleak
```

**Usage:**
```bash
python ble_fen_client.py
```

The script scans for `SmartChess-ESP32S3`, connects, subscribes to FEN/LOG notifications, and accepts commands from stdin.

---

## Documentation

Detailed technical documentation is in the `docs/` directory:

| File | Contents |
|------|----------|
| [`01-overview.md`](docs/01-overview.md) | System goals and high-level capabilities |
| [`02-hardware-and-build.md`](docs/02-hardware-and-build.md) | Hardware pinout, PlatformIO configuration, directory layout |
| [`03-firmware-workflow.md`](docs/03-firmware-workflow.md) | Game lifecycle, START/STOP flow, command reference |
| [`04-scan-state-machine.md`](docs/04-scan-state-machine.md) | Full state machine description and transition conditions |
| [`05-ble-protocol.md`](docs/05-ble-protocol.md) | GATT UUIDs, characteristic properties, ACK format |
| [`06-web-client.md`](docs/06-web-client.md) | JavaScript implementation, ACK handling, UI reset policy |
| [`07-python-client-and-debug.md`](docs/07-python-client-and-debug.md) | Python client usage and troubleshooting guide |
| [`08-limitations-and-roadmap.md`](docs/08-limitations-and-roadmap.md) | Known limitations and proposed improvements |

---

## Known Limitations

| Area | Limitation |
|------|-----------|
| Move validation | No check, pin, or checkmate detection — a full legal-move engine is not embedded |
| Promotion | Pawns auto-promote to Queen; underpromotion is not supported |
| Castling | Castling rights are tracked in FEN but executing castling requires moving pieces individually |
| Memory | UIDs stored as `String` objects; long sessions may incur heap fragmentation |

---

## Roadmap

**High priority**
- Persist UID→piece mapping in NVS to eliminate re-scanning on every START
- N-sample hysteresis for improved false-lift rejection

**Medium priority**
- Replace `String` UIDs with fixed-size `char[8]` buffers to eliminate heap fragmentation
- Lightweight legal-move validator (check/pin detection)

**Low priority**
- Automated integration tests for ACK flow and state machine transitions
- Multi-piece detection to support castling as a single gesture

---

## License

MIT — see [LICENSE](LICENSE) for details.
