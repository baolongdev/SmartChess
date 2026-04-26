# 01 — System Overview

## What Is SmartChess?

SmartChess is an embedded system that adds real-time move tracking and wireless connectivity to a standard physical chess board. Each piece is fitted with an RFID tag; an 8×8 antenna matrix driven by an ESP32-S3 reads which squares are occupied and which UIDs are present. Firmware translates antenna readings into game events, maintains full board state, and broadcasts the position as a standard FEN string over Bluetooth Low Energy.

## Goals

| Goal | Implementation |
|------|----------------|
| Track all 64 squares in real time | 64-antenna multiplexed matrix via 12 GPIO pins |
| Minimize unnecessary scanning | Candidate-list filtering — only relevant squares are scanned per tick |
| Produce standard chess notation | 6-field FEN (placement, active color, castling, en-passant, half-move, full-move) |
| Wireless game state delivery | BLE GATT with three characteristics: FEN feed, command channel, log stream |
| Control session lifecycle | `START` / `STOP` commands over BLE or serial |
| Live browser visualization | Web client using the Web Bluetooth API |

## Current Capabilities

- **Session guard:** Tracking only begins after a successful `START` that confirms all 32 starting pieces.
- **Startup validation:** Scans 32 starting squares, checks every UID is readable, rejects duplicate UIDs.
- **Candidate-list tracking:** Move destinations are searched only within the legal moves of the lifted piece.
- **Turn enforcement:** Lifting the wrong color piece is rejected immediately — before any destination search begins.
- **False-lift debouncing:** A brief debounce window (`SCAN_LIFT_PENDING`) filters transient RFID read failures.
- **Strict ACK protocol:** Every BLE command response is prefixed with `OK:` or `ERR:` for unambiguous client parsing.
- **UI recovery:** The web client automatically resets to default state on START failure, STOP, or BLE disconnect.

## System Block Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                        ESP32-S3                             │
│                                                             │
│   ┌──────────────┐   SPI    ┌────────────┐                 │
│   │ Antenna      │◄────────►│  MFRC522   │                 │
│   │ Matrix       │          │  RFID      │                 │
│   │ (64 coils)   │          │  Reader    │                 │
│   └──────────────┘          └────────────┘                 │
│                                                             │
│   ┌─────────────────────────────────────────────────────┐  │
│   │  SmartChessApp (state machine + game logic)         │  │
│   │   ├─ RfidScanner   (antenna control + UID reads)    │  │
│   │   ├─ MoveGen       (candidate square generation)    │  │
│   │   ├─ Fen           (FEN string construction)        │  │
│   │   └─ BleFen        (BLE GATT server)                │  │
│   └─────────────────────────────────────────────────────┘  │
│                          │ BLE                              │
└──────────────────────────┼──────────────────────────────────┘
                           │
          ┌────────────────┼────────────────┐
          │                │                │
   ┌──────▼──────┐  ┌──────▼──────┐  ┌─────▼──────┐
   │ Web Client  │  │   Python    │  │   Any BLE  │
   │ (browser)   │  │   Client    │  │   Client   │
   └─────────────┘  └─────────────┘  └────────────┘
```

## Related Documents

- [Hardware and Build](02-hardware-and-build.md)
- [Firmware Workflow](03-firmware-workflow.md)
- [BLE Protocol](05-ble-protocol.md)
