# 04 — Scan State Machine

The RFID scan loop is driven by a six-state machine that advances on every `smartChessTick()` call. The machine is only active when `gameStarted = true`.

## States

| State | Purpose |
|-------|---------|
| `SCAN_IDLE` | Monitor occupied squares for any UID disappearance |
| `SCAN_LIFT_PENDING` | Debounce — confirm the lift is real, not a transient read failure |
| `SCAN_PIECE_LIFTED` | Enforce turn order and generate candidate destination list |
| `SCAN_TRACKING_DESTINATION` | Scan candidate squares to find where the piece lands |
| `SCAN_VERIFY` | Require N stable rounds before committing the move |
| `SCAN_FALLBACK` | Attempt recovery when the destination was not found in time |

---

## State Transition Diagram

```
                    ┌────────────────────────────────────────────────┐
                    │                  SCAN_IDLE                     │
                    │  Scan occupiedList only                        │
                    └───────────────────────┬────────────────────────┘
                                            │ UID missing from occupied square
                                            ▼
                    ┌────────────────────────────────────────────────┐
                    │              SCAN_LIFT_PENDING                 │
                    │  Re-scan source square for DEBOUNCE_LIFT_MS   │
                    └──────┬──────────────────────────┬─────────────┘
          UID returns /    │                          │  Square still empty
          new UID appears  │                          │  after debounce
                    ▼      │                          ▼
             SCAN_IDLE     │         ┌────────────────────────────────┐
                           │         │       SCAN_PIECE_LIFTED        │
                           │         │  Check turn; generate          │
                           │         │  candidateList via MoveGen     │
                           │         └──────────┬─────────────────────┘
                           │    wrong turn       │  correct turn
                           │         ▼           ▼
                           │    SCAN_FALLBACK   SCAN_TRACKING_DESTINATION
                           │                    │  scan source + candidates
                           │         UID found  │                │  TRACKING_TIMEOUT_MS
                           │                    ▼                ▼
                           │              SCAN_VERIFY       SCAN_FALLBACK
                           │              │ VERIFY_ROUNDS        │ recovery attempts
                           │              │ stable               │
                           │              ▼                      │ found
                           │         applyMove()            SCAN_VERIFY
                           │         publish FEN                 │
                           │              │                      │ FALLBACK_TIMEOUT_MS
                           │              ▼                      ▼
                           └─────── SCAN_IDLE              abandon move
                                                            prompt re-place
```

---

## State Details

### SCAN_IDLE

**What it does:** On each tick, scans only the squares in `occupiedList` (squares currently known to have a piece). This avoids scanning all 64 squares every loop.

**Transition:** When any occupied square returns no UID, the missing square becomes the lift candidate and the machine moves to `SCAN_LIFT_PENDING`.

---

### SCAN_LIFT_PENDING

**What it does:** Protects against transient RFID read failures that briefly drop a UID for one or two scan cycles.

**Behavior:**
1. Re-scan the suspect square.
2. If the original UID reappears → treat as glitch, return to `SCAN_IDLE`.
3. If a *different* UID appears → update source UID, return to `SCAN_IDLE`.
4. If the square remains empty for the full `DEBOUNCE_LIFT_MS` window → confirm as a real lift.

**Log output:**
```
[INFO] LIFT_GLITCH at e2 | scan glitch ignored
```

---

### SCAN_PIECE_LIFTED

**What it does:** Validates the lift and sets up tracking.

**Behavior:**
1. Re-check the source square one final time to rule out late-returning glitches.
2. **Turn enforcement:** Compare the lifted piece's color against `whiteTurn`.
   - If wrong turn → log `ERR WRONG_TURN` and transition to `SCAN_FALLBACK`.
3. If correct turn → remove the UID from `occupiedList`, call `MoveGen::generateCandidateSquares()`.

**Log output:**
```
[LIFT] e2 piece lifted
[ERR] WRONG_TURN expected=white got=black
```

---

### SCAN_TRACKING_DESTINATION

**What it does:** Watches the source square and all candidate squares for the lifted UID.

**Behavior:**
1. Scan the source square plus every square in `candidateList`.
2. If the lifted UID appears at a candidate square → transition to `SCAN_VERIFY`.
3. If `TRACKING_TIMEOUT_MS` elapses without finding the UID → transition to `SCAN_FALLBACK`.

**Log output:**
```
[TRACK] TARGET d4
```

---

### SCAN_VERIFY

**What it does:** Confirms the move is physically stable before committing it.

**Behavior:**
1. Requires `VERIFY_ROUNDS` consecutive scans (default: 2) where:
   - Source square is empty, AND
   - Destination square returns the lifted UID.
2. On success → call `applyMove()`, publish updated FEN, return to `SCAN_IDLE`.
3. If conditions are not met within a scan → decrement counter and re-scan, or return to tracking.

**Log output:**
```
[OK] MOVE_CONFIRMED e2->e4
```

---

### SCAN_FALLBACK

**What it does:** Attempts to recover from tracking failures (timeout, wrong-turn abandon, or unexpected piece behavior).

**Behavior:**
1. First priority: check if the piece was returned to the source square.
2. Second priority: scan `candidateList` again.
3. Third priority: scan the full `occupiedList` for the UID.
4. If found anywhere → transition to `SCAN_VERIFY`.
5. If `FALLBACK_TIMEOUT_MS` elapses without finding the UID → abandon the move and prompt the player to replace the piece.

**Log output:**
```
[ERR] TRACK_TIMEOUT
[ERR] FALLBACK_TIMEOUT — please replace piece
```

---

## Timing Constants

| Constant | Default | Defined in |
|----------|---------|-----------|
| `DEBOUNCE_LIFT_MS` | 50 ms | `SmartChessConfig.h` |
| `TRACKING_TIMEOUT_MS` | 3 000 ms | `SmartChessConfig.h` |
| `FALLBACK_TIMEOUT_MS` | 5 000 ms | `SmartChessConfig.h` |
| `FALLBACK_RETRY_DELAY` | 150 ms | `SmartChessConfig.h` |
| `VERIFY_ROUNDS` | 2 | `SmartChessConfig.h` |

---

## Related Documents

- [Firmware Workflow](03-firmware-workflow.md)
- [Hardware and Build](02-hardware-and-build.md)
