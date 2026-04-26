# 08 — Limitations and Roadmap

## Known Limitations

### 1. No Full Legal Move Validation

**What is missing:** The firmware does not check whether a move leaves the king in check, respects absolute pins, or results in checkmate or stalemate.

**Impact:** Illegal moves (e.g., moving a pinned piece, moving into check) are accepted by the firmware.

**Workaround:** Pair with an external engine (e.g., Stockfish via the web client) if strict legality enforcement is required.

---

### 2. Pawn Promotion — Queen Only

**What is missing:** When a pawn reaches the back rank, the firmware automatically promotes it to a Queen. Underpromotion (Rook, Bishop, Knight) is not supported.

**Impact:** Positions requiring underpromotion cannot be accurately represented.

---

### 3. Castling Requires Sequential Moves

**What is missing:** The firmware processes one piece lift at a time. Castling — which involves moving both the king and the rook — must be performed as two separate moves.

**Impact:** Castling rights are tracked correctly in the FEN, but the physical gesture of moving both pieces simultaneously will confuse the state machine.

**Workaround:** Move the king first, then the rook. The firmware will recognize each as a separate move.

---

### 4. UID Storage Uses Dynamic Strings

**What is affected:** UIDs are stored as Arduino `String` objects, which allocate from the heap.

**Impact:** Extended sessions may accumulate heap fragmentation, potentially causing instability on very long games.

**Current mitigation:** PSRAM is enabled on the ESP32-S3, providing a much larger heap than SRAM alone.

---

## Roadmap

### High Priority

**1. Persist UID→Piece Mapping in NVS**

Store the UID-to-piece mapping in Non-Volatile Storage after a successful START. On subsequent power cycles, the firmware can skip the scan phase and enter tracking immediately.

- Benefit: Eliminates the need to re-scan every time the board is powered on.
- Effort: Medium — NVS API is straightforward on ESP32.

**2. N-Sample Hysteresis for Lift Detection**

Instead of a single debounce window, require N consecutive empty reads before confirming a lift.

- Benefit: Further reduces false-lift events in electrically noisy environments.
- Effort: Low — change to `DEBOUNCE_LIFT_MS` logic in `SmartChessApp.cpp`.

---

### Medium Priority

**3. Fixed-Size UID Buffers**

Replace `String` UIDs with `char uid[8]` or `uint8_t uid[7]` fixed-size arrays.

- Benefit: Eliminates heap fragmentation; deterministic memory usage.
- Effort: Medium — requires updating all UID comparison, storage, and logging code.

**4. Lightweight Legal Move Validator**

Add a check-detection layer that verifies the king is not left in check after each candidate move is applied.

- Benefit: Prevents illegal moves without requiring a full chess engine.
- Effort: High — requires a bitboard or mailbox board representation for fast attack generation.

---

### Low Priority

**5. Automated Integration Tests**

Add host-side tests for:
- ACK format compliance (`OK:` / `ERR:` prefix).
- Turn enforcement (wrong-turn rejection).
- State machine transitions (idle → lift → track → verify → apply).

- Benefit: Catches regressions without manual board testing.
- Effort: Medium — requires a BLE test harness or serial mock.

**6. Multi-Piece Detection (Castling Gesture)**

Detect when two pieces are lifted in quick succession and interpret the pattern as castling.

- Benefit: Natural castling gesture support.
- Effort: High — state machine redesign required to handle two concurrent lift contexts.

---

## Related Documents

- [System Overview](01-overview.md)
- [Scan State Machine](04-scan-state-machine.md)
- [Firmware Workflow](03-firmware-workflow.md)
