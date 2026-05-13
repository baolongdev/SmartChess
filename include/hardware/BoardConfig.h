#pragma once

// ---------------------------------------------------------------------------
// BoardConfig.h — Hardware pin assignments, antenna map, and timing constants
// for the SmartChess ESP32-S3 board.
// ---------------------------------------------------------------------------

#include <Arduino.h>

// SPI pins (MFRC522)
constexpr int SCK_PIN  = 14;
constexpr int MISO_PIN = 21;
constexpr int MOSI_PIN = 2;
constexpr int SS_PIN   = 1;
constexpr int RST_PIN  = 48;

// Board geometry
constexpr int NUM_ANTENNAS  = 64;   // 8×8 squares
constexpr int MAX_PIECES    = 32;
constexpr int MAX_CANDIDATES = 28;

// Timing
constexpr unsigned long SCAN_PRINT_INTERVAL_MS  = 350;
constexpr unsigned long TIMING_PRINT_INTERVAL_MS = 1000;
constexpr unsigned long DEBOUNCE_LIFT_MS         = 50;
constexpr unsigned long TRACKING_TIMEOUT_MS      = 3000;
constexpr unsigned long FALLBACK_TIMEOUT_MS      = 5000;
constexpr unsigned long FALLBACK_RETRY_DELAY_MS  = 150;
constexpr unsigned long IDLE_SLEEP_MS            = 2;
// SCAN_WAIT_RESTORE: how often to scan for the missing piece returning
constexpr unsigned long WAIT_RESTORE_SCAN_MS     = 400;
// SCAN_WAIT_RESTORE: how often to reprint the expected board (reminder)
constexpr unsigned long WAIT_RESTORE_PRINT_MS    = 8000;
constexpr int           VERIFY_ROUNDS            = 2;

// GPIO pin pairs for antenna multiplexing (6 odd + 6 even = 64 combinations)
constexpr int ODD_PINS[6]  = {6, 15, 17, 8, 10, 12};
constexpr int EVEN_PINS[6] = {7, 16, 18, 9, 11, 13};

// Antenna bit-pattern lookup table (6-bit, one per square)
constexpr uint8_t ANTENNA_ARRAY[NUM_ANTENNAS] = {
  // --- a1..a8 ---
  0b000000, 0b110000, 0b001000, 0b111000, 0b000100, 0b110100, 0b001100, 0b111100,
  // --- b1..b8 ---
  0b100000, 0b010000, 0b101000, 0b011000, 0b100100, 0b010100, 0b101100, 0b011100,
  // --- c1..c8 ---
  0b111111, 0b011111, 0b110111, 0b010111, 0b100010, 0b000010, 0b101010, 0b001010,
  // --- d1..d8 ---
  0b001111, 0b101111, 0b000111, 0b100111, 0b010010, 0b110010, 0b011010, 0b111010,
  // --- e1..e8 ---
  0b111011, 0b011011, 0b110011, 0b010011, 0b100110, 0b000110, 0b101110, 0b001110,
  // --- f1..f8 ---
  0b001011, 0b101011, 0b000011, 0b100011, 0b010110, 0b110110, 0b011110, 0b111110,
  // --- g1..g8 ---
  0b011101, 0b101101, 0b010101, 0b100101, 0b011001, 0b101001, 0b010001, 0b100001,
  // --- h1..h8 ---
  0b111101, 0b001101, 0b110101, 0b000101, 0b111001, 0b001001, 0b110001, 0b000001,
};
