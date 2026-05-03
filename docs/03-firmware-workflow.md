# 03 — Firmware Workflow

## Game Lifecycle

Firmware operates in two phases: idle and active game.

```
Power on
  └─► smartChessBegin()
        ├─ Initialize RFID scanner
        ├─ Start BLE GATT server
        ├─ Start WiFi (if SSID saved in NVS)
        ├─ Start BoardRegistration heartbeat (5 s fast poll)
        └─ Enter idle state (gameStarted = false)

Receive START (BLE, Serial, or WiFi heartbeat command)
  └─► startAndLearnInitial32()
        ├─ fail → boardRegQueueScanResult("MISSING", "a1,h8,...")
        │         or boardRegQueueScanResult("DUPLICATE", "")
        │         respond ERR: START_FAILED:...  (stay idle)
        └─ pass → boardRegQueueScanResult("STARTED", "32")
                   map UIDs, enable state machine
                   respond OK: STARTED
                   (gameStarted = true)

Active game — smartChessTick() called every loop()
  ├─► RFID state machine advances each tick
  │     ├─ Confirmed move → applyMove() → publish FEN via BLE + WebPublish
  │     ├─ In-game error → boardRegQueueAlert(code, detail)
  │     └─ Keepalive timer → republish FEN every 1.5 s
  └─► boardRegTick() — heartbeats + queued scan results + queued alerts

Receive STOP
  └─► Disable state machine, clear game state
        respond OK: STOPPED
        (gameStarted = false)
```

---

## START Command

`START` performs the following steps in sequence:

1. **Reset runtime state** — clears board model, turn flag, castling rights, en-passant square, move counters.
2. **Scan 32 starting squares** — only known starting positions are scanned (not all 64).
3. **Validate completeness** — every starting square must return a readable UID.
4. **Validate uniqueness** — no two squares may share the same UID.
5. **Build UID→piece mapping** — each UID is associated with its piece character.
6. **Queue scan result** — `boardRegQueueScanResult(result, detail)` (sent to server on next tick).
7. **Enable tracking** — sets `gameStarted = true` and activates the scan state machine.

### START Response

| Outcome                           | BLE/Serial response                         | Scan result sent to server                    |
|-----------------------------------|---------------------------------------------|-----------------------------------------------|
| All 32 pieces present and unique  | `OK: STARTED`                               | `result: "STARTED"`, `detail: "32"`           |
| One or more squares unreadable    | `ERR: START_FAILED:MISSING:<square-list>`   | `result: "MISSING"`, `detail: "a1,h8,..."`    |
| Duplicate UIDs detected           | `ERR: START_FAILED:DUPLICATE:<detail>`      | `result: "DUPLICATE"`, `detail: ""`           |

> **Note**: scan result codes are `"MISSING"` and `"DUPLICATE"` — not `"MISSING_PIECES"` or `"DUPLICATE_UID"`.

---

## WiFi START Flow

When a game is created on the web UI with a physical board selected, the server queues a `START` command for delivery:

```
Web UI: POST /games { boardID: "ESP32-..." }
  → Server sets pendingCommands.set(gameID, "START")

ESP32 heartbeat (5 s):
  POST /boards/heartbeat → response: { ok: true, gameID: "...", command: "START" }
  → firmware sets pendingWifiStart = true
  → next smartChessTick() calls startAndLearnInitial32()
  → result posted to POST /boards/scan-result
  → server emits board_scan_ok or board_scan_failed to the web client
```

The `command` field in the heartbeat response is **one-shot** — cleared on first delivery, never repeated.

---

## In-Game Error Alerts

During an active game, the firmware queues alerts to the server whenever the RFID state machine encounters an error:

```cpp
boardRegQueueAlert("WRONG_TURN",        "expected=white got=black at e7");
boardRegQueueAlert("NO_LEGAL_DEST",     "from=e2");
boardRegQueueAlert("TRACK_TIMEOUT",     "from=e2");
boardRegQueueAlert("ILLEGAL_DEST",      "from=e2 to=e5");
boardRegQueueAlert("PIECE_LOST",        "from=e2 uid=A1B2C3D4");
boardRegQueueAlert("FALLBACK_TIMEOUT",  "from=e2");
```

Alerts are sent on the next `boardRegTick()` call via `POST /boards/alert`. The web client receives a `board_alert` socket event and shows a toast notification.

---

## STOP Command

`STOP` immediately halts tracking and resets game state:
- Sets `gameStarted = false`
- Resets scan state machine to `SCAN_IDLE`
- Clears lifted-piece context and candidate list
- Returns `OK: STOPPED`

---

## Command Reference

### BLE CMD Characteristic

| Command                                  | Description               | Success                              | Error                                             |
|------------------------------------------|---------------------------|--------------------------------------|---------------------------------------------------|
| `START`                                  | Begin game session        | `OK: STARTED`                        | `ERR: START_FAILED:MISSING:...` or `ERR: START_FAILED:DUPLICATE:...` |
| `STOP`                                   | End game session          | `OK: STOPPED`                        | —                                                 |
| `STATUS`                                 | Query session state       | `OK: RUNNING` or `OK: IDLE`          | —                                                 |
| `F` / `FEN`                              | Request current FEN       | `OK: <fen>`                          | `ERR: NO_GAME`                                    |
| `WEB_SET <url> <boardID> <gameID> <0\|1>` | Configure web server     | `OK: ...`                            | —                                                 |
| `WEB_STATUS`                             | Print web config          | `OK: ...`                            | —                                                 |
| `WIFI_SET <ssid> <pass>`                 | Set WiFi credentials      | `OK: WIFI_SET`                       | —                                                 |
| `WIFI_CONNECT`                           | Connect to saved WiFi     | `OK: WIFI_CONNECTING`                | —                                                 |
| `WIFI_STATUS`                            | Print WiFi state          | `OK: ...`                            | —                                                 |
| `WIFI_SCAN`                              | Scan nearby networks      | `OK: NETWORKS ...`                   | —                                                 |
| `OTA_BEGIN`                              | Start OTA firmware flash  | `OK: OTA_BEGIN`                      | —                                                 |
| *(other)*                                | —                         | —                                    | `ERR: UNKNOWN_CMD`                                |

### Serial Shell

Available via `pio device monitor` (115200 baud). Commands are auto-committed after 120 ms idle.

| Command      | Description                         |
|--------------|-------------------------------------|
| `START`      | Same as BLE START                   |
| `STOP`       | Same as BLE STOP                    |
| `STATUS`     | Print current game status           |
| `F` / `FEN`  | Print current FEN                   |
| `S` / `SNAP` | Full 64-square manual scan (debug)  |
| `V`          | Toggle verbose RFID output          |
| `C`          | Toggle continuous board print       |
| `L`          | Toggle piece letter labels          |
| `T`          | Print scan timing info              |
| `B`          | Print BLE connection status         |
| `HELP`       | List all commands                   |

---

## Move Application

When a move is confirmed by the state machine (`applyMove()`):

1. Source square UID removed from `occupiedList` / `squareUID[]`.
2. Destination square UID set to the moved piece's UID.
3. Captured piece UID removed.
4. Castling rights updated if king or rook moved.
5. En-passant square set for double pawn push.
6. Half-move clock incremented (reset on pawn move or capture).
7. Full-move number incremented after Black's move.
8. `whiteTurn` toggled.
9. New FEN generated and published via BLE + `webMoveTick()`.

---

## Related Documents

- [Scan State Machine](04-scan-state-machine.md)
- [BLE Protocol](05-ble-protocol.md)
- [Web Integration](09-web-integration.md)
- [Hardware and Build](02-hardware-and-build.md)
