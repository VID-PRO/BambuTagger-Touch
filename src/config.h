#pragma once
/*
 * config.h — User-editable defaults (compile-time fallbacks)
 *
 * Modeled after BambuTagger-Console/src/config.h.
 *
 * Changing these values requires a recompile.  Runtime overrides
 * (WiFi credentials, preferences, etc.) are stored in NVS.
 */

// ── Version info ──────────────────────────────────────────────
#define APP_NAME    "BambuTagger-Touch"
#define APP_VERSION "2.0.0"

// ── Debug ─────────────────────────────────────────────────────
//  Set DEBUG_SERIAL to 0 to strip all Serial debug output from
//  the build (saves flash / improves speed).
#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 1
#endif

// ── WiFi Access Point (fallback when no WiFi configured) ──────
#define AP_SSID "BambuTagger-Touch"
#define HOSTNAME "BambuTagger-Touch"
#define AP_PASS "bambu1234"

// ── OTA / Firmware updates ────────────────────────────────────
#define FIRMWARE_VERSION "1.9.6"
#define OTA_REPO         "VID-PRO/BambuTagger-Touch"

// ── GitHub dump source (queengooborg/Bambu-Lab-RFID-Library) ──
#define GITHUB_API_HOST "api.github.com"
#define GITHUB_RAW_HOST "raw.githubusercontent.com"
#define GITHUB_REPO_PATH "/queengooborg/Bambu-Lab-RFID-Library"
#define GITHUB_RAW_PREFIX "https://raw.githubusercontent.com/queengooborg/Bambu-Lab-RFID-Library/main/"

// ── RFID (RC522 on HSPI) ──────────────────────────────────────
#define PIN_RFID_CS  18
#define PIN_RFID_RST 17

// ── MIFARE Classic 1K layout ──────────────────────────────────
#define MIFARE_BLOCKS      64
#define BYTES_PER_BLOCK    16
#define DUMP_SIZE          (MIFARE_BLOCKS * BYTES_PER_BLOCK)  // 1024
#define NUM_SECTORS        16
#define BLOCKS_PER_SECTOR  4

// ── NDEF parser limits ────────────────────────────────────────
#define NDEF_MAX_RECORDS  4
#define NDEF_MAX_PAYLOAD  320

// ── NTAG tag layout ───────────────────────────────────────────
#define NTAG_MAX_PAGES     48
#define TT_USER_PAGE_START  4
#define TT_USER_BYTE_START (TT_USER_PAGE_START * 4)     // = 16
#define TT_MIN_READ_PAGES  (TT_USER_PAGE_START + 25)    // 29

// ── Display (ESP32-8048S043 / Guition JC8048W550, 800×480) ───
#define LCD_WIDTH   800
#define LCD_HEIGHT  480

// ── Console-inspired colour palette (RGB565) ──────────────────
#define COL_BG       0x18C5  // #1A1A2E  very dark navy      — screen background
#define COL_CARD     0x1107  // #16213E  slightly lighter     — card / container
#define COL_SIDEBAR  0x09AC  // #0F3460  medium dark blue     — status bar / footer
#define COL_ACC      0x1DCA  // #1DB954  Bambu green accent
#define COL_TEXT     0xEF5D  // #EAEAEA  near-white text
#define COL_SUBTEXT  0x8CD5  // #8899AA  grey-blue muted text
#define COL_RED      0xE267  // #E74C3C  error / failed
#define COL_ORANGE   0xF524  // #F5A623  warning / paused
#define COL_GREY     0x6BAF  // #6C757D  muted grey buttons
#define COL_PROGRESS 0x29AC  // #2D3561  progress bar track
#define COL_SLATE    0x320A  // #334155  slate UI elements
#define COL_BLUE     0x34DB  // #3498DB  selection highlight / progress fill
