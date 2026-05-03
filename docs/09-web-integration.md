# 09 — Web Integration (WiFi + BoardRegistration)

The firmware integrates with the BEChessWeb server over WiFi using three HTTP endpoints. All web communication is handled by `src/net/BoardRegistration.cpp`.

---

## Configuration

Web integration is configured via BLE or Serial:

```
WEB_SET <serverUrl> <boardID> <gameID> <enabled(0|1)>

# Example:
WEB_SET http://192.168.1.100:8080 ESP32-AA:BB:CC  1
```

All values are persisted to NVS (flash) so they survive power cycles.

Fields:
- `serverUrl` — Base URL of the BEChessWeb server (e.g. `http://192.168.1.100:8080`)
- `boardID` — Unique board identifier, typically the ESP32 MAC address
- `gameID` — (optional) Pre-assigned game ID; usually left blank and assigned by the server
- `enabled` — `1` to enable web moves publishing; `0` to disable

---

## Heartbeat — `POST /boards/heartbeat`

Sent by `boardRegTick()` on a timer. Interval:
- **5 s** — fast poll (while `gameStarted = false`, waiting for game assignment)
- **30 s** — normal poll (during active game)

**Request body**:
```json
{
  "boardID": "ESP32-AA:BB:CC",
  "gameID":  "game_alpha",
  "ip":      "192.168.1.42"
}
```

**Response** (the server may include optional fields):
```json
{
  "ok":      true,
  "gameID":  "game_alpha",
  "command": "START"
}
```

Response handling:
- `gameID` present and different → update `gCurrentGameID` and call `gGameIDCallback`
- `command` present → call `gCommandCallback` (sets `pendingWifiStart = true` for `"START"`)

The `command` field is **one-shot**: the server clears it from `pendingCommands` immediately after sending. The ESP32 will never receive the same command twice from the same heartbeat cycle.

---

## Scan Result — `POST /boards/scan-result`

Sent immediately (one-shot) when `boardRegQueueScanResult(result, detail)` is called, on the next `boardRegTick()`.

```json
{
  "boardID": "ESP32-AA:BB:CC",
  "gameID":  "game_alpha",
  "result":  "STARTED",
  "detail":  "32"
}
```

`result` values:
| Value         | Meaning                | `detail`                                       |
|---------------|------------------------|------------------------------------------------|
| `"STARTED"`   | All 32 pieces valid    | `"32"`                                         |
| `"MISSING"`   | Unreadable squares     | Comma-separated square names, e.g. `"a1,h8"`  |
| `"DUPLICATE"` | Duplicate RFID tag     | `""` or UID string                             |

The server uses this to update the game's `status` field and emit `board_scan_ok` / `board_scan_failed` to the web client.

---

## In-Game Alert — `POST /boards/alert`

Sent immediately (one-shot) when `boardRegQueueAlert(code, detail)` is called, on the next `boardRegTick()`.

```json
{
  "boardID": "ESP32-AA:BB:CC",
  "gameID":  "game_alpha",
  "code":    "WRONG_TURN",
  "detail":  "expected=white got=black at e7"
}
```

Alert codes:

| Code                | Trigger point                                                     |
|---------------------|-------------------------------------------------------------------|
| `WRONG_TURN`        | `handlePieceLifted()` — wrong side lifted a piece                 |
| `NO_LEGAL_DEST`     | `handlePieceLifted()` — no legal moves from lifted square         |
| `TRACK_TIMEOUT`     | `handleTrackingDestination()` — piece not placed within timeout   |
| `ILLEGAL_DEST`      | `handleVerify()` — piece placed on non-legal square               |
| `PIECE_LOST`        | `handleFallback()` — RFID tag stopped responding                  |
| `FALLBACK_TIMEOUT`  | `handleFallback()` — fallback recovery timed out                  |

The server relays this as a `board_alert` socket event to the web client, which shows a toast notification.

---

## One-Shot Queue Pattern

Both scan results and alerts use the same one-shot queue pattern:

```cpp
// Queue (called from state machine):
boardRegQueueScanResult("MISSING", "a1,h8");
// or
boardRegQueueAlert("WRONG_TURN", "from=e2");

// Delivery (called from boardRegTick() every loop):
void boardRegTick() {
    if (gScanResult.length() > 0) doScanResult();  // clears gScanResult after POST
    if (gAlertCode.length() > 0)  doAlert();        // clears gAlertCode after POST
    // ... heartbeat interval logic ...
}
```

If WiFi is disconnected when the tick fires, the queued message is **discarded** (cleared without sending). This prevents a backlog of stale events.

---

## Full WiFi Game Flow

```
1. Board powers on, connects to WiFi
2. Heartbeat begins (fast, 5 s)

3. User opens web UI, selects board, enters player names, clicks "Start"
   → POST /games { boardID, WhiteName, BlackName }
   → Server: game created (status: "waiting_scan"), pendingCommands.set(gameID, "START")
   → Server: emits board_heartbeat { boardID, gameID, online: true }

4. Web client navigates to /board?id=... — shows "Scanning pieces..." overlay
   Web client emits socket.emit("join", { gameID }) immediately

5. Next heartbeat arrives at ESP32 (≤ 5 s):
   → Response: { ok: true, gameID: "...", command: "START" }
   → pendingWifiStart = true

6. Next smartChessTick():
   → startAndLearnInitial32()

   Case A — 32 pieces found:
     → boardRegQueueScanResult("STARTED", "32")
     → gameStarted = true
     → Next boardRegTick(): POST /boards/scan-result { result: "STARTED" }
     → Server: saveGame(status: "active"), emits board_scan_ok + game_status_update
     → Web client: ScanStateView disappears → live game shown

   Case B — pieces missing:
     → boardRegQueueScanResult("MISSING", "a1,...")
     → Next boardRegTick(): POST /boards/scan-result { result: "MISSING", detail: "a1,..." }
     → Server: saveGame(status: "scan_failed"), emits board_scan_failed
     → Web client: ScanStateView shows "Missing at: a1" + Rescan button

7. User clicks Rescan on web:
   → POST /games/:id/rescan
   → Server: status back to "waiting_scan", pendingCommands.set(gameID, "START") again
   → Server: emits game_status_update { status: "waiting_scan" }
   → Web client: ScanStateView switches back to scanning spinner
   → Loop from step 5

8. Active game: moves published via POST /moves or WebPublish
   Errors → boardRegQueueAlert() → POST /boards/alert → web toast
```

---

## BoardRegistration API (firmware-internal)

```cpp
// Initialise (call once after WiFi is ready):
boardRegBegin(serverUrl, boardID, gameID);

// Update config at runtime (after WEB_SET command):
boardRegSetConfig(serverUrl, boardID);
boardRegSetGameID(gameID);

// Callbacks:
boardRegSetGameIDCallback([](const String &id) { webGameID = id; });
boardRegSetCommandCallback([](const String &cmd) {
    if (cmd == "START") pendingWifiStart = true;
});

// Call every loop() (non-blocking):
boardRegTick();

// Queue a scan result (call once after startAndLearnInitial32()):
boardRegQueueScanResult("STARTED", "32");
boardRegQueueScanResult("MISSING", "a1,h8");
boardRegQueueScanResult("DUPLICATE", "");

// Queue an in-game alert (call from state machine on error):
boardRegQueueAlert("WRONG_TURN", "from=e2");

// Force an immediate heartbeat on next tick:
boardRegForceHeartbeat();

// Switch between fast (5 s) and normal (30 s) heartbeat intervals:
boardRegSetFastPoll(true);   // while waiting for game start
boardRegSetFastPoll(false);  // during active game
```

---

## Related Documents

- [Firmware Workflow](03-firmware-workflow.md)
- [BLE Protocol](05-ble-protocol.md)
- [BEChessWeb REST API](../../BEChessWeb/docs/06-api-rest.md)
- [BEChessWeb Socket Events](../../BEChessWeb/docs/07-api-socket.md)
