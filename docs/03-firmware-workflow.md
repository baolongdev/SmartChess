# 03 — Firmware Workflow

## Game Lifecycle

Firmware operates in two phases:

```
Power on
  └─► smartChessBegin()
        ├─ Initialize RFID scanner
        ├─ Start BLE GATT server
        └─ Enter idle state (gameStarted = false)

Receive START
  └─► Validate starting position (32 squares)
        ├─ fail → respond ERR: START_FAILED:...  (stay idle)
        └─ pass → map UIDs, enable state machine
                   respond OK: STARTED
                   (gameStarted = true)

Active game — smartChessTick() called every loop()
  └─► State machine advances each tick
        ├─ Confirmed move → applyMove() → publish FEN
        └─ Keepalive timer → republish FEN every 1.5 s

Receive STOP
  └─► Disable state machine, clear game state
        respond OK: STOPPED
        (gameStarted = false)
```

---

## START Command

`START` performs the following steps in sequence:

1. **Reset runtime state** — clears board model, turn flag, castling rights, en-passant square, move counters.
2. **Scan 32 starting squares** — only the known starting positions are scanned (not all 64 squares).
3. **Validate completeness** — every starting square must return a readable UID.
4. **Validate uniqueness** — no two squares may share the same UID.
5. **Build UID→piece mapping** — each UID is associated with its piece character for later lookup.
6. **Enable tracking** — sets `gameStarted = true` and activates the scan state machine.

### START Response

| Outcome | Response |
|---------|----------|
| All 32 pieces present and unique | `OK: STARTED` |
| One or more squares unreadable | `ERR: START_FAILED:MISSING_PIECES:<square-list>` |
| Duplicate UIDs detected | `ERR: START_FAILED:DUPLICATE_UID:<detail>` |

Serial log on success:
```
[READY] STARTED | pieces=32
```

---

## STOP Command

`STOP` immediately halts tracking and resets all game state:

- Sets `gameStarted = false`
- Resets the scan state machine to `SCAN_IDLE`
- Clears the lifted-piece context and candidate list
- Returns `OK: STOPPED`

---

## Command Reference

### BLE CMD Characteristic

Sent by writing to the CMD characteristic (UUID `...0003`). The firmware responds by notifying the same characteristic.

| Command | Description | Success ACK | Error ACK |
|---------|-------------|-------------|-----------|
| `START` | Begin game session | `OK: STARTED` | `ERR: START_FAILED:...` |
| `STOP` | End game session | `OK: STOPPED` | — |
| `STATUS` | Query session state | `OK: RUNNING` or `OK: IDLE` | — |
| `F` / `FEN` | Request current FEN | `OK: <fen>` | `ERR: NO_GAME` |
| *(other)* | — | — | `ERR: UNKNOWN_CMD` |

### Serial Shell

Available via `pio device monitor` (115200 baud). Commands are auto-committed after `CMD_COMMIT_IDLE_MS` (120 ms) of idle input — pressing Enter is not required.

| Command | Description |
|---------|-------------|
| `START` | Same as BLE START |
| `STOP` | Same as BLE STOP |
| `STATUS` | Print current game status |
| `F` / `FEN` | Print current FEN string |
| `S` / `SNAP` | Full 64-square manual scan (debug) |
| `V` | Toggle verbose RFID output |
| `C` | Toggle continuous board print |
| `L` | Toggle piece letters vs. dots in board dump |
| `T` | Print scan timing information |
| `B` | Print BLE connection status |
| `HELP` | List all available commands |

---

## Move Application

When a move is confirmed by the state machine (`applyMove()` is called):

1. Source square UID is removed from `occupiedList`.
2. Destination square UID is set to the moved piece's UID.
3. If a capture occurred, the captured piece's UID is removed.
4. Castling rights are updated if a king or rook moved.
5. En-passant square is set if a pawn advanced two squares.
6. Half-move clock increments (resets to 0 on pawn move or capture).
7. Full-move number increments after Black's move.
8. Active color flips (`whiteTurn` toggles).
9. A new FEN string is generated and published via BLE.

---

## Related Documents

- [Scan State Machine](04-scan-state-machine.md)
- [BLE Protocol](05-ble-protocol.md)
- [Hardware and Build](02-hardware-and-build.md)
