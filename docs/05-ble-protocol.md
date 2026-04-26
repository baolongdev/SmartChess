# 05 — BLE Protocol

## GATT Service

| Field | Value |
|-------|-------|
| Device advertising name | `SmartChess-ESP32S3` |
| Service UUID | `3f0e0001-70a1-4f8a-a6a3-51e9590e9f20` |

## Characteristics

| Name | UUID | Properties | Description |
|------|------|------------|-------------|
| FEN | `3f0e0002-70a1-4f8a-a6a3-51e9590e9f20` | READ, NOTIFY | Current board position as a 6-field FEN string |
| CMD | `3f0e0003-70a1-4f8a-a6a3-51e9590e9f20` | READ, WRITE, NOTIFY | Command input / ACK output |
| LOG | `3f0e0004-70a1-4f8a-a6a3-51e9590e9f20` | READ, NOTIFY | Real-time runtime log stream |

---

## CMD Characteristic

### Writing Commands

Write a UTF-8 string to the CMD characteristic. The firmware parses and executes the command, then notifies the same characteristic with the ACK.

Supported commands: `START`, `STOP`, `STATUS`, `F`, `FEN`.

### ACK Format

Every response is prefixed with `OK: ` or `ERR: ` — no exceptions. Clients must check this prefix before parsing the payload.

```
OK: <payload>
ERR: <error-code>[:<detail>]
```

### Command/ACK Reference

| Command | Success ACK | Error ACK |
|---------|-------------|-----------|
| `START` | `OK: STARTED` | `ERR: START_FAILED:MISSING_PIECES:e2,f7` |
| `START` | — | `ERR: START_FAILED:DUPLICATE_UID:<uid>` |
| `STOP` | `OK: STOPPED` | — |
| `STATUS` | `OK: RUNNING` | — |
| `STATUS` | `OK: IDLE` | — |
| `F` / `FEN` | `OK: rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1` | `ERR: NO_GAME` |
| *(unrecognized)* | — | `ERR: UNKNOWN_CMD` |

---

## FEN Characteristic

### Content

The FEN characteristic always holds a valid 6-field FEN string, e.g.:

```
rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1
```

Before a game is started, the characteristic contains `-`.

### Update Triggers

| Trigger | Behavior |
|---------|----------|
| Confirmed move (`applyMove`) | FEN is recalculated and notified immediately |
| Keepalive timer | FEN is re-notified every `BLE_KEEPALIVE_MS` (1 500 ms) while a client is connected |
| Initial connection | Client can READ the characteristic to get the current FEN at any time |

The keepalive ensures clients that miss a NOTIFY can recover without manual intervention.

---

## LOG Characteristic

The LOG characteristic streams human-readable status messages from the firmware in real time. Clients subscribe via NOTIFY.

### Log Prefixes

| Prefix | Meaning |
|--------|---------|
| `[READY]` | Session started successfully |
| `[INFO]` | Informational event (glitch ignored, etc.) |
| `[LIFT]` | Piece lift detected |
| `[TRACK]` | Destination tracking update |
| `[MOVE]` | Move applied |
| `[OK]` | Operation succeeded |
| `[ERR]` | Error condition |
| `[CMD]` | Command received and result |

### Example Log Sequence

```
[CMD] START => OK: STARTED
[LIFT] e2 piece lifted
[TRACK] TARGET e4
[OK] MOVE_CONFIRMED e2->e4
[CMD] F => OK: rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1
```

---

## Client Implementation Notes

- Subscribe to NOTIFY on all three characteristics before sending `START`.
- Always parse the `OK:` / `ERR:` prefix before acting on a CMD response.
- If `NOTIFY` is missed, use `READ` on the FEN characteristic to recover current state.
- The 1.5 s keepalive means clients should see a FEN refresh at least every 2 s when connected.
- The device must be disconnected before a new `START` can be issued if a session is already `RUNNING`.

---

## Related Documents

- [Firmware Workflow](03-firmware-workflow.md)
- [Web Client](06-web-client.md)
- [Python Client and Debug](07-python-client-and-debug.md)
