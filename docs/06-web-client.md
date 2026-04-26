# 06 — Web Client

## Overview

The web client is a single-page application that runs in a desktop browser and connects to the SmartChess firmware directly via the **Web Bluetooth API**. No installation, drivers, or native app is required.

```
web-client/
├── index.html    # Layout: board, controls, FEN display, log panel
├── styles.css    # Terminal-style dark theme
└── app.js        # Web Bluetooth logic, ACK handling, board rendering
```

## Features

| Feature | Description |
|---------|-------------|
| BLE connect / disconnect | Scan for `SmartChess-ESP32S3` and pair with one click |
| Session control | START / STOP buttons with confirmation feedback |
| Live board | 8×8 board rendered from each FEN notification; last move highlighted |
| FEN history | Browse previous positions with forward/back navigation |
| Board controls | Flip orientation, copy FEN to clipboard, export position history |
| Real-time log | LOG characteristic notifications displayed in a scrollable panel |

## Getting Started

Web Bluetooth requires either HTTPS or `localhost`. The simplest approach is a local HTTP server:

```bash
python -m http.server 8080
```

Then open `http://localhost:8080/web-client/` in Chrome or Edge.

**Supported browsers:**

| Browser | Support |
|---------|---------|
| Chrome 56+ (desktop) | Full support |
| Edge 79+ (desktop) | Full support |
| Firefox | Not supported |
| Safari | Not supported |
| Chrome (Android) | Partial — depends on OS version |

## ACK Handling

The client applies strict ACK validation to avoid acting on noise or stale notifications.

### Validation Rules

1. **Prefix check:** An ACK is only accepted if it starts with `OK: ` or `ERR: `. Any other content (startup strings, noise) is treated as invalid.

2. **Retry on invalid:** If an invalid response is received, the client retries up to **4 times** before giving up and synthesizing `ERR: ACK_INVALID:<received-content>`.

3. **Race condition guard:** The CMD characteristic reference is snapshot-captured before any async operation to prevent null-dereference errors during unexpected disconnects:
   ```javascript
   const cmdChar = cmdCharacteristic;  // snapshot before await
   ```

### ACK Parsing Flow

```
Write command to CMD characteristic
  └─► Wait for NOTIFY on CMD
        ├─ starts with "OK: "  → extract payload, resolve promise
        ├─ starts with "ERR: " → extract error, reject promise
        └─ neither             → retry (up to 4 times)
                                  └─ all retries exhausted → ERR: ACK_INVALID
```

## UI Reset Policy

The client UI resets to a clean default state under the following conditions:

| Event | UI Behavior |
|-------|-------------|
| `START` command fails | Reset session state; reset board to default FEN |
| `STOP` command succeeds | Reset board; clear FEN history |
| User-initiated BLE disconnect | Reset board; clear FEN history |
| Unexpected BLE disconnect | Reset board; clear FEN history; display reconnect warning |

This ensures the UI never shows stale game state from a previous session.

## Board Rendering

The board re-renders on every FEN notification from the BLE FEN characteristic. Rendering steps:

1. Parse the piece-placement field of the FEN string.
2. Map each piece character to a Unicode chess symbol.
3. Highlight the source and destination squares of the last confirmed move.
4. Apply board orientation (normal or flipped).

---

## Related Documents

- [BLE Protocol](05-ble-protocol.md)
- [Firmware Workflow](03-firmware-workflow.md)
- [Python Client and Debug](07-python-client-and-debug.md)
