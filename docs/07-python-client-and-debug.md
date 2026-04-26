# 07 — Python Client and Debugging

## Python BLE Client

`ble_fen_client.py` is a command-line BLE client for testing SmartChess without a browser. It uses the [bleak](https://github.com/hbldh/bleak) library for cross-platform BLE access.

### Installation

```bash
pip install bleak
```

### Usage

**Connect and subscribe to FEN notifications (continuous):**
```bash
python ble_fen_client.py
```

**Read the current FEN once and exit:**
```bash
python ble_fen_client.py --read-once
```

**List nearby BLE devices:**
```bash
python ble_fen_client.py --list
```

**Connect to a specific device by MAC address:**
```bash
python ble_fen_client.py --address XX:XX:XX:XX:XX:XX
```

**All options:**

| Flag | Description |
|------|-------------|
| `--address <mac>` | Connect to a specific MAC address instead of scanning by name |
| `--duration <seconds>` | Listen for this many seconds (0 = indefinite) |
| `--print-duplicates` | Print notifications even when FEN has not changed |
| `--scan-timeout <seconds>` | BLE scan timeout when searching for the device |
| `--name <name>` | Override the device name to search for |
| `--service-uuid <uuid>` | Override the service UUID |
| `--char-uuid <uuid>` | Override the FEN characteristic UUID |

---

## Serial Shell

While `pio device monitor` is running (115200 baud), the following debug commands are available. Commands are auto-committed after 120 ms of idle input — pressing Enter is not required.

| Command | Description |
|---------|-------------|
| `START` | Begin game (same as BLE) |
| `STOP` | End game (same as BLE) |
| `STATUS` | Print current session state |
| `F` / `FEN` | Print current FEN string |
| `S` / `SNAP` | Manual full 64-square scan |
| `V` | Toggle verbose RFID read output |
| `C` | Toggle continuous board print each scan cycle |
| `L` | Toggle piece letters vs. dots in board dump |
| `T` | Print per-antenna scan timing |
| `B` | Print BLE connection status |
| `HELP` | List all commands |

---

## Log Reference

### Normal Operation Logs

| Log Message | Meaning |
|-------------|---------|
| `[READY] STARTED \| pieces=32` | START succeeded; all 32 pieces mapped |
| `[LIFT] e2 piece lifted` | Piece lift confirmed on square e2 |
| `[TRACK] TARGET e4` | Tracking destination — UID found on e4 |
| `[OK] MOVE_CONFIRMED e2->e4` | Move applied; FEN updated |
| `[CMD] START => OK: STARTED` | Command acknowledged successfully |
| `[CMD] STOP => OK: STOPPED` | Session stopped |

### Error Logs

| Log Message | Meaning | Action |
|-------------|---------|--------|
| `[ERR] WRONG_TURN expected=white got=black` | Player lifted the wrong color's piece | Wait for the correct turn |
| `[ERR] TRACK_TIMEOUT at e2` | Destination not found within 3 s | Place the piece on a valid square |
| `[ERR] FALLBACK_TIMEOUT` | Recovery period expired | Manually return the piece to its source square |
| `[ERR] NO_LEGAL_DEST for <piece>` | No candidate moves generated | Check piece type and board state |
| `[INFO] LIFT_GLITCH at e2` | Transient UID loss filtered out | No action needed |
| `ERR: START_FAILED:MISSING_PIECES:e2,f7` | UID unreadable on listed squares | Adjust piece placement; check tag contact |
| `ERR: START_FAILED:DUPLICATE_UID:<uid>` | Two pieces share the same RFID UID | Replace the duplicate tag |
| `ERR: ACK_INVALID:...` | Web client received a malformed ACK | Check BLE stability; reconnect |

---

## Troubleshooting

### START Fails

**Symptoms:** `ERR: START_FAILED:MISSING_PIECES:...` or `DUPLICATE_UID`

**Causes and fixes:**

| Cause | Fix |
|-------|-----|
| Piece not in starting position | Place all 32 pieces in the standard starting arrangement |
| Antenna not reading tag | Ensure the piece is centered on the square; check tag-to-coil gap (should be < 2 cm) |
| Two pieces with the same UID | Replace the affected RFID tag |
| Power supply instability | Use a stable 3.3 V regulated supply; add decoupling capacitor near MFRC522 |

### Frequent False Lifts

**Symptoms:** `[INFO] LIFT_GLITCH` messages appear frequently; moves triggered without physical movement.

**Causes and fixes:**

| Cause | Fix |
|-------|-----|
| RF interference | Add shielding around the antenna matrix; keep away from other wireless devices |
| Tag too far from coil | Ensure pieces sit flat on the board; reduce tag-coil distance |
| Power noise | Add 100 nF decoupling capacitors on the MFRC522 power lines |

If false lifts persist, increase `DEBOUNCE_LIFT_MS` in `SmartChessConfig.h`.

### Web Client Receives `ACK_INVALID`

**Symptoms:** Browser console shows `ERR: ACK_INVALID`; START/STOP buttons appear to hang.

**Causes and fixes:**

| Cause | Fix |
|-------|-----|
| CMD characteristic not yet ready | Disconnect, wait 2–3 s, reconnect |
| Firmware sent a startup string before game begins | The retry mechanism should handle this; check Serial log for CMD flow |
| BLE connection is unstable | Move the device closer to the ESP32-S3 |

### Move Not Detected

**Symptoms:** Piece is moved physically but FEN does not update.

**Checklist:**
1. Confirm a game is `RUNNING` (`STATUS` command).
2. Verify it is the correct player's turn.
3. Check Serial log for `[LIFT]` messages — if absent, the antenna is not reading the tag.
4. Run `S` / `SNAP` to manually scan all 64 squares and inspect which UIDs are detected.

---

## Related Documents

- [BLE Protocol](05-ble-protocol.md)
- [Web Client](06-web-client.md)
- [Scan State Machine](04-scan-state-machine.md)
