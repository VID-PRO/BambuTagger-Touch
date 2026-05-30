/*
 * ==================================================================
 *  BambuTagger-Touch — Guition JC8048W550 + RC522 + Touchscreen
 * ==================================================================
 *  Read, clone and write Bambu Lab filament spool RFID tags.
 *  Dump library:  https://github.com/queengooborg/Bambu-Lab-RFID-Library
                    https://bambuman.ee/
 *  Tag format:    https://github.com/queengooborg/Bambu-Lab-RFID-Tag-Guide
                    https://github.com/Bambu-Research-Group/RFID-Tag-Guide
 *
 *  KEY DERIVATION (KDF):
 *    Keys = HKDF-SHA256(IKM=UID[4], salt=BAMBU_KDF_SALT[16],
 *                       info="RFID-A\0" or "RFID-B\0", L=96)
 *    → 16 × 6-byte sector keys  (one per MIFARE sector)
 *
 *  HARDWARE (Guition JC8048W550 / ESP32-S3-WROOM-1-N16R8):
 *  ┌──────────────────────────────────────────────────────────┐
 *  │  5.0" 800×480 RGB TFT via ST7262 (parallel RGB565)       │
 *  │    R0-R4: 8,3,46,9,1      G0-G5: 5,6,7,15,16,4           │
 *  │    B0-B4: 45,48,47,21,14  DE:40  VS:41  HS:39  PCLK:42   │
 *  │    Backlight → GPIO 2                                    │
 *  │    Touch: GT911 on I2C (SDA=19, SCL=20, RST=38)          │
 *  ├──────────────────────────────────────────────────────────┤
 *  │  RC522 (SPI on HSPI bus)                                 │
 *  │    SDA/CS → GPIO 18    SCK  → GPIO 12                    │
 *  │    MOSI   → GPIO 11    MISO → GPIO 13                    │
 *  │    RST    → GPIO 17    3V3  / GND                        │
 *  │    Touch INT shares GPIO 18 (not used by code)           │
 *  └──────────────────────────────────────────────────────────┘
 *
 *  REQUIRED LIBRARIES (Arduino Library Manager):
 *    • LovyanGFX             (lovyan03)
 *    • makerspaceleiden/rfid (github.com/makerspaceleiden/rfid)
 *    • ArduinoJson           (Benoit Blanchon)
 *    mbedTLS is bundled with the ESP32 Arduino core.
 *
 *  WRITE OPERATIONS NEED A "MAGIC" / UID-CHANGEABLE MIFARE CARD
 *    (CUID / FUID / Gen2) so that block 0 (UID) can be written.
 *    Plain factory MIFARE Classic 1K cards keep their UID fixed
 *    and will only have blocks 1-63 updated.
 *
 *  CLONE vs WRITE-FROM-DUMP:
 *    • Clone     – read a live Bambu tag, write raw data to
 *                  a blank magic card (preserving UID).
 *    • Write Dump– download a pre-scanned .bin from GitHub
 *                  via the built-in web interface, store on FAT,
 *                  then write to a card.
 * ============================================================
 */

// ──────────────────────────────────────────────────────────────
//  Includes
// ──────────────────────────────────────────────────────────────
#include "splash_jpg.h"
#include "logo_bitmap.h"
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LovyanGFX.hpp>
#include <miniz.h>

// Include platform-specific RGB bus/panel headers (ESP32-S3)
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

// ── LovyanGFX config for ESP32-8048S043C (ST7262 RGB + GT911) ─
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_RGB   _bus;
  lgfx::Panel_RGB _panel;
  lgfx::Touch_GT911 _touch;
public:
  LGFX(void) {
    {
      auto cfg = _bus.config();
      cfg.pin_d0  =  8; cfg.pin_d1  =  3; cfg.pin_d2  = 46; cfg.pin_d3  =  9; cfg.pin_d4  =  1;
      cfg.pin_d5  =  5; cfg.pin_d6  =  6; cfg.pin_d7  =  7; cfg.pin_d8  = 15; cfg.pin_d9  = 16;
      cfg.pin_d10 =  4; cfg.pin_d11 = 45; cfg.pin_d12 = 48; cfg.pin_d13 = 47; cfg.pin_d14 = 21; cfg.pin_d15 = 14;
      cfg.pin_henable = 40; cfg.pin_vsync = 41; cfg.pin_hsync = 39; cfg.pin_pclk = 42;
      cfg.freq_write = 15000000;
      cfg.hsync_pulse_width = 4;   cfg.hsync_back_porch  = 20;  cfg.hsync_front_porch = 16;
      cfg.vsync_pulse_width = 4;   cfg.vsync_back_porch  = 4;  cfg.vsync_front_porch = 8;
      cfg.pclk_idle_high = true;
      cfg.panel = &_panel;
      _bus.config(cfg);
    }
    _panel.setBus(&_bus);
    {
      auto pcfg = _panel.config();
      pcfg.memory_width  = 800; pcfg.memory_height = 480;
      pcfg.panel_width   = 800; pcfg.panel_height  = 480;
      pcfg.offset_x = 0; pcfg.offset_y = 0;
      pcfg.rgb_order = 1;
      _panel.config(pcfg);
    }
    {
      auto tcfg = _touch.config();
      tcfg.pin_int = -1; tcfg.pin_rst = 38;
      tcfg.x_min = 0; tcfg.x_max = 479; tcfg.y_min = 0; tcfg.y_max = 799;
      tcfg.i2c_port = 0;
      tcfg.i2c_addr = 0x5D;
      tcfg.pin_sda = 19; tcfg.pin_scl = 20;
      tcfg.freq = 400000;
      _touch.config(tcfg);
    }
    _panel.setTouch(&_touch);
    setPanel(&_panel);
  }
};
LGFX lcd;

// ── Debug output ──────────────────────────────────────────────
//  Set DEBUG_SERIAL to 0 to strip all debug prints from the build.
#define DEBUG_SERIAL 1
#if DEBUG_SERIAL
#define DBG(...) Serial.print(__VA_ARGS__)
#define DBGLN(...) Serial.println(__VA_ARGS__)
#define DBGF(...) Serial.printf(__VA_ARGS__)
#else
#define DBG(...)
#define DBGLN(...)
#define DBGF(...)
#endif
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <FFat.h>
#include <Update.h>
#include "mbedtls/md.h"
#include <vector>

// ──────────────────────────────────────────────────────────────
//  Pin definitions
// ──────────────────────────────────────────────────────────────
// ESP32-8048S043  (ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB PSRAM)
// RFID SPI bus (CS=18, SCK=12, MOSI=11, MISO=13) on HSPI.
#define PIN_RFID_CS 18
#define PIN_RFID_RST 17

// ──────────────────────────────────────────────────────────────
//  Constants
// ──────────────────────────────────────────────────────────────
#define LCD_WIDTH  800
#define LCD_HEIGHT 480

#define MIFARE_BLOCKS 64  // MIFARE Classic 1K
#define BYTES_PER_BLOCK 16
#define DUMP_SIZE (MIFARE_BLOCKS * BYTES_PER_BLOCK)  // 1024
#define NUM_SECTORS 16
#define BLOCKS_PER_SECTOR 4

#define AP_SSID "BambuTagger"
#define AP_PASS "bambu1234"

#define FIRMWARE_VERSION "1.9.3"          // bumped by release workflow tag
#define OTA_REPO         "VID-PRO/BambuTagger-Touch"

#define GITHUB_API_HOST "api.github.com"
#define GITHUB_RAW_HOST "raw.githubusercontent.com"
#define GITHUB_REPO_PATH "/queengooborg/Bambu-Lab-RFID-Library"
#define GITHUB_RAW_PREFIX "https://raw.githubusercontent.com/queengooborg/Bambu-Lab-RFID-Library/main/"

// Bambu Lab HKDF salt (from reverse-engineered KDF)
static const uint8_t BAMBU_KDF_SALT[16] = {
  0x9a, 0x75, 0x9c, 0xf2, 0xc4, 0xf7, 0xca, 0xff,
  0x22, 0x2c, 0xb9, 0x76, 0x9b, 0x41, 0xbc, 0x96
};



// ──────────────────────────────────────────────────────────────
//  Tag data
// ──────────────────────────────────────────────────────────────
struct TagInfo {
  uint8_t uid[4];
  char filamentType[17];  // block 2
  char detailedType[17];  // block 4
  char variantId[9];      // block 1 bytes 0-7
  char materialId[9];     // block 1 bytes 8-15
  uint8_t colorR, colorG, colorB;
  uint16_t spoolWeight;     // grams
  float diameter;           // mm
  uint16_t minNozzleTemp;   // °C
  uint16_t maxNozzleTemp;   // °C
  uint16_t bedTemp;         // °C
  uint16_t dryTemp;         // °C
  uint16_t dryTime;         // hours
  uint16_t filamentLength;  // metres
  uint8_t raw[MIFARE_BLOCKS][BYTES_PER_BLOCK];
  bool valid;
};

TagInfo currentTag;  // most recently read
TagInfo sourceTag;   // for clone operation
uint8_t dumpBuf[DUMP_SIZE];
char selectedDumpPath[64] = "";
bool    g_webWrite       = false;          // true when /api/writetag triggered the write
void  (*g_writeSectorCb)(int, int) = nullptr; // (sectDone, sectTotal) progress callback

// Pages to display for a read tag


// ──────────────────────────────────────────────────────────────
//  Global objects
// ──────────────────────────────────────────────────────────────
SPIClass rfidSPI(HSPI);
MFRC522_SPI rfid_spi(PIN_RFID_CS, PIN_RFID_RST, &rfidSPI, SPISettings(1000000, MSBFIRST, SPI_MODE0));
MFRC522 rfid(&rfid_spi);
WebServer httpServer(80);
Preferences prefs;

// ──────────────────────────────────────────────────────────────
//  Application state machine
// ──────────────────────────────────────────────────────────────
enum AppState {
  S_MAIN_MENU,
  S_READ_TAG,
  S_SHOW_TAG,
  S_CLONE_SOURCE,
  S_CLONE_TARGET,
  S_DUMP_SELECT,
  S_DUMP_WRITE,
  S_WIFI_INFO,
  S_GH_BROWSE,     // GitHub OLED browser
  S_GH_DOWNLOAD,   // downloading dump file to FAT
  S_BM_BROWSE,     // BambuMan OLED browser (waiting for tag)
  S_BM_DOWNLOAD,   // BambuMan fetch in progress
  S_BM_CAT_BROWSE, // BambuMan catalog 4-level OLED browser
  S_OTA_UPDATE,    // OTA firmware update flow
  S_GEN4_MANAGE    // Gen4 card management (Seal / Unlock)
};
AppState appState = S_MAIN_MENU;

// ──────────────────────────────────────────────────────────────
//  Menu
// ──────────────────────────────────────────────────────────────
static const char* MENU_ITEMS[] = {
  "Read Tag",
  "Clone Tag",
  "Write Tag",
  "GitHub Lib",
  "BambuMan Lib",
  "Tag Tool",
  "System",
  "OTA Update"
};
static const int MENU_COUNT = 8;
int menuSel = 0;
int menuScroll = 0;
String bmFetchUid = "";  // UID fetched from BambuMan

// Forward declarations for display helpers used before definition
static void drawBtn(int x, int y, int w, int h, uint16_t bg, const char* label);
static void drawStatusBar();
static void drawSubHeader(const char* title);
static void drawFooter();
void showStatus(const char* msg);
static void countDumpFiles(const String& path, int& count);
void drawProgressBar(int pct, const char* phase, const char* label);
bool bmCatLoadLevel();
void drawBmCatBrowser();

// ──────────────────────────────────────────────────────────────
//  HKDF-SHA256  (RFC 5869)
// ──────────────────────────────────────────────────────────────
static void hmacSHA256(const uint8_t* key, size_t kLen,
                       const uint8_t* data, size_t dLen,
                       uint8_t* out32) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, md, 1);
  mbedtls_md_hmac_starts(&ctx, key, kLen);
  mbedtls_md_hmac_update(&ctx, data, dLen);
  mbedtls_md_hmac_finish(&ctx, out32);
  mbedtls_md_free(&ctx);
}

// Generate `okmLen` bytes of keying material
static void hkdf256(const uint8_t* ikm, size_t ikmLen,
                    const uint8_t* salt, size_t saltLen,
                    const uint8_t* info, size_t infoLen,
                    uint8_t* okm, size_t okmLen) {
  // Extract
  uint8_t prk[32];
  hmacSHA256(salt, saltLen, ikm, ikmLen, prk);

  // Expand
  uint8_t T[32] = { 0 };
  size_t tLen = 0;
  uint8_t ctr = 0;
  size_t done = 0;

  while (done < okmLen) {
    ctr++;
    // input = T(i-1) || info || ctr
    size_t inLen = tLen + infoLen + 1;
    uint8_t* input = (uint8_t*)malloc(inLen);
    if (!input) return;
    if (tLen) memcpy(input, T, tLen);
    memcpy(input + tLen, info, infoLen);
    input[tLen + infoLen] = ctr;

    hmacSHA256(prk, 32, input, inLen, T);
    free(input);
    tLen = 32;

    size_t n = min((size_t)32, okmLen - done);
    memcpy(okm + done, T, n);
    done += n;
  }
}

/* Derive all 16 A-keys and 16 B-keys from a 4-byte UID.
   keysA[s][0..5] = sector-s Key-A
   keysB[s][0..5] = sector-s Key-B                         */
void bambuDeriveKeys(const uint8_t uid[4],
                     uint8_t keysA[16][6],
                     uint8_t keysB[16][6]) {
  // "RFID-A\0" and "RFID-B\0" – 7 bytes including the null
  static const uint8_t INFO_A[7] = { 'R', 'F', 'I', 'D', '-', 'A', '\0' };
  static const uint8_t INFO_B[7] = { 'R', 'F', 'I', 'D', '-', 'B', '\0' };

  uint8_t okm[96];

  hkdf256(uid, 4, BAMBU_KDF_SALT, 16, INFO_A, 7, okm, 96);
  for (int i = 0; i < 16; i++) memcpy(keysA[i], okm + i * 6, 6);

  hkdf256(uid, 4, BAMBU_KDF_SALT, 16, INFO_B, 7, okm, 96);
  for (int i = 0; i < 16; i++) memcpy(keysB[i], okm + i * 6, 6);
}

// ──────────────────────────────────────────────────────────────
//  Tag parsing helpers
// ──────────────────────────────────────────────────────────────
static void trimStr(char* s, int maxLen) {
  for (int i = maxLen - 1; i >= 0; i--) {
    if (s[i] == '\0' || s[i] == ' ') s[i] = '\0';
    else break;
  }
}

static void parseTagBlocks(TagInfo* t) {
  // Filament type  – block 2
  memcpy(t->filamentType, t->raw[2], 16);
  t->filamentType[16] = '\0';
  trimStr(t->filamentType, 16);

  // Detailed type  – block 4
  memcpy(t->detailedType, t->raw[4], 16);
  t->detailedType[16] = '\0';
  trimStr(t->detailedType, 16);

  // Variant ID     – block 1 bytes 0-7
  memcpy(t->variantId, t->raw[1], 8);
  t->variantId[8] = '\0';
  trimStr(t->variantId, 8);

  // Material ID    – block 1 bytes 8-15
  memcpy(t->materialId, t->raw[1] + 8, 8);
  t->materialId[8] = '\0';
  trimStr(t->materialId, 8);

  // Color (BGRA, block 5 bytes 0-3)
  t->colorR = t->raw[5][0];
  t->colorG = t->raw[5][1];
  t->colorB = t->raw[5][2];

  // Spool weight   – block 5 bytes 4-5 (little-endian uint16)
  t->spoolWeight = (uint16_t)t->raw[5][4] | ((uint16_t)t->raw[5][5] << 8);

  // Diameter       – block 5 bytes 8-11 (float LE)
  memcpy(&t->diameter, t->raw[5] + 8, 4);

  // Temperatures   – block 6
  t->dryTemp = (uint16_t)t->raw[6][0] | ((uint16_t)t->raw[6][1] << 8);
  t->dryTime = (uint16_t)t->raw[6][2] | ((uint16_t)t->raw[6][3] << 8);
  t->bedTemp = (uint16_t)t->raw[6][6] | ((uint16_t)t->raw[6][7] << 8);
  t->maxNozzleTemp = (uint16_t)t->raw[6][8] | ((uint16_t)t->raw[6][9] << 8);
  t->minNozzleTemp = (uint16_t)t->raw[6][10] | ((uint16_t)t->raw[6][11] << 8);

  // Filament length – block 14 bytes 4-5
  t->filamentLength = (uint16_t)t->raw[14][4] | ((uint16_t)t->raw[14][5] << 8);
}

// Copy a TagInfo's raw blocks into a flat 1024-byte dump buffer
static void tagToFlat(const TagInfo* t, uint8_t* buf) {
  for (int b = 0; b < MIFARE_BLOCKS; b++)
    memcpy(buf + b * BYTES_PER_BLOCK, t->raw[b], BYTES_PER_BLOCK);
}

// Fill a TagInfo from a flat dump buffer
static void flatToTag(const uint8_t* buf, TagInfo* t) {
  memset(t, 0, sizeof(TagInfo));
  for (int b = 0; b < MIFARE_BLOCKS; b++)
    memcpy(t->raw[b], buf + b * BYTES_PER_BLOCK, BYTES_PER_BLOCK);
  memcpy(t->uid, buf, 4);
  parseTagBlocks(t);
  t->valid = true;
}

// ──────────────────────────────────────────────────────────────
//  RFID operations
// ──────────────────────────────────────────────────────────────
static bool tryAuth(int blockAddr, MFRC522::MIFARE_Key* key, bool useKeyA) {
  uint8_t cmd = useKeyA ? MFRC522::PICC_CMD_MF_AUTH_KEY_A
                        : MFRC522::PICC_CMD_MF_AUTH_KEY_B;
  bool ok = rfid.PCD_Authenticate(cmd, blockAddr, key, &rfid.uid) == MFRC522::STATUS_OK;
  DBGF("[AUTH]  blk=%02d key%s %02X%02X%02X%02X%02X%02X -> %s\n",
       blockAddr, useKeyA ? "A" : "B",
       key->keyByte[0], key->keyByte[1], key->keyByte[2],
       key->keyByte[3], key->keyByte[4], key->keyByte[5],
       ok ? "OK" : "FAIL");
  if (!ok) {
    // A failed auth can leave the card in HALT/IDLE state.
    // Re-select so the next auth attempt starts with a clean ACTIVE-state card.
    if (!rfidReSelect()) {
      DBGLN("[AUTH]  re-select after auth fail — card removed");
    }
  }
  return ok;
}

/* Read all 64 blocks of a Bambu Lab tag.
   Authenticates each sector using derived keys, falls back to
   the default 0xFF…FF key for blank/overwritten sectors.
   Returns true if at least the first data sector was readable. */
bool rfidReadBambuTag(TagInfo* t) {
  memset(t, 0, sizeof(TagInfo));
  t->valid = false;

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
    return false;

  if (rfid.uid.size < 4) {
    rfid.PICC_HaltA();
    return false;
  }
  memcpy(t->uid, rfid.uid.uidByte, 4);
  DBGF("[RFID] UID: %02X %02X %02X %02X\n",
       t->uid[0], t->uid[1], t->uid[2], t->uid[3]);

  uint8_t keysA[16][6], keysB[16][6];
  bambuDeriveKeys(t->uid, keysA, keysB);
  DBGLN("[RFID] Key derivation complete.");

  MFRC522::MIFARE_Key mk;
  bool anyRead = false;

  for (int sec = 0; sec < NUM_SECTORS; sec++) {
    int trailer = sec * BLOCKS_PER_SECTOR + 3;

    // Build auth key objects
    MFRC522::MIFARE_Key kA, kB, kDef;
    memcpy(kA.keyByte, keysA[sec], 6);
    memcpy(kB.keyByte, keysB[sec], 6);
    memset(kDef.keyByte, 0xFF, 6);

    bool authed = tryAuth(trailer, &kA, true)
                  || tryAuth(trailer, &kB, false)
                  || tryAuth(trailer, &kDef, true)
                  || tryAuth(trailer, &kDef, false);
    DBGF("[READ]  sector %02d auth -> %s\n", sec, authed ? "OK" : "FAIL");
    if (!authed) continue;

    for (int b = 0; b < BLOCKS_PER_SECTOR; b++) {
      int addr = sec * BLOCKS_PER_SECTOR + b;
      uint8_t buf[18];
      uint8_t sz = 18;
      if (rfid.MIFARE_Read(addr, buf, &sz) == MFRC522::STATUS_OK) {
        memcpy(t->raw[addr], buf, BYTES_PER_BLOCK);
        anyRead = true;
        DBGF("[READ]    blk %02d: %02X %02X %02X %02X %02X %02X %02X %02X"
             " %02X %02X %02X %02X %02X %02X %02X %02X\n",
             addr,
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
             buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
      } else {
        DBGF("[READ]    blk %02d: read FAIL\n", addr);
      }
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (!anyRead) {
    DBGLN("[READ] No blocks readable – aborting.");
    return false;
  }
  parseTagBlocks(t);
  t->valid = true;
  DBGF("[READ] Tag OK  type=%s  color=#%06X  wt=%.0fg\n",
       t->filamentType,
       ((uint32_t)t->colorR << 16) | ((uint32_t)t->colorG << 8) | t->colorB,
       t->spoolWeight);
  return true;
}

// ──────────────────────────────────────────────────────────────
//  Gen1A ("Chinese magic card") backdoor support
//  These cards accept a special 0x40/0x43 unlock command that
//  bypasses all sector authentication, allowing direct block writes.
// ──────────────────────────────────────────────────────────────

/* Attempt the Gen1A unlock sequence on the currently-selected card.
   Sends 0x40 at 7-bit frame, then 0x43 at 8-bit frame.
   Returns true if the card acknowledges both (Gen1A detected).
   Safe to call on any card; a normal card will NAK/ignore → returns false. */
static bool gen1aUnlock() {
  rfid.PCD_StopCrypto1();

  // Step 1 — 0x40 with 7-bit frame, no CRC
  {
    byte cmd     = 0x40;
    byte resp[4]; byte respLen = sizeof(resp);
    byte vBits   = 7;   // 7 significant bits in the first (only) byte
    auto s = rfid.PCD_TransceiveData(&cmd, 1, resp, &respLen, &vBits, 0, false);
    rfid.PCD_WriteRegister(MFRC522::BitFramingReg, 0x00); // always restore framing
    if (s != MFRC522::STATUS_OK) return false;
    if ((resp[0] & 0x0F) != 0x0A) return false;  // expect 4-bit MIFARE ACK
  }

  // Step 2 — 0x43 with 8-bit frame, no CRC
  {
    byte cmd     = 0x43;
    byte resp[4]; byte respLen = sizeof(resp);
    auto s = rfid.PCD_TransceiveData(&cmd, 1, resp, &respLen, nullptr, 0, false);
    if (s != MFRC522::STATUS_OK) return false;
    if ((resp[0] & 0x0F) != 0x0A) return false;
  }

  return true;  // card is now in Gen1A backdoor mode
}

/* Write one 16-byte block on a Gen1A-unlocked card (no auth required).
   Uses the standard MIFARE Write command — the card accepts it without auth
   because it is in backdoor mode. */
static bool gen1aWriteBlock(uint8_t blockAddr, const uint8_t* data16) {
  MFRC522::StatusCode s = rfid.MIFARE_Write(blockAddr, (byte*)data16, 16);
  return s == MFRC522::STATUS_OK;
}

/* Re-select the card after a failed magic-detection command that may have sent
   it back to IDLE/HALT state.  Halts, waits briefly, then re-polls.
   Returns true if the card is back in ACTIVE state with a valid UID. */
static bool rfidReSelect() {
  // After PICC_HaltA() the card enters ISO14443A HALT state and only wakes
  // via WUPA (0x52), but PICC_IsNewCardPresent() sends REQA (0x26) which the
  // halted card ignores.  Cycling the RF field is the most reliable fix: it
  // power-cycles the card to IDLE so it responds to the next REQA normally.
  rfid.PCD_StopCrypto1();
  rfid.PCD_AntennaOff();
  delay(30);                        // card capacitor drains → IDLE state
  rfid.PCD_AntennaOn();
  delay(20);                        // RF field stabilises, card powers up
  for (uint8_t i = 0; i < 8; i++) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) return true;
    delay(25);                      // 8 × 25 ms = up to 200 ms total window
  }
  return false;
}

/* Send a raw ISO14443A command with CRC-A appended; check CRC of the reply.
   cmd / cmdLen : command bytes WITHOUT CRC.
   resp / respLen: caller buffer; *respLen = capacity on entry, bytes received on exit.
   Returns true on STATUS_OK (transceive + CRC check both passed). */
static bool rfidRawCmd(const uint8_t* cmd, uint8_t cmdLen,
                       uint8_t* resp, uint8_t* respLen) {
  if ((uint16_t)cmdLen + 2u > 32u) return false;
  uint8_t pkt[32];
  memcpy(pkt, cmd, cmdLen);
  byte crc[2];
  if (rfid.PCD_CalculateCRC(pkt, cmdLen, crc) != MFRC522::STATUS_OK) return false;
  pkt[cmdLen]     = crc[0];
  pkt[cmdLen + 1] = crc[1];
  rfid.PCD_StopCrypto1();
  MFRC522::StatusCode s = rfid.PCD_TransceiveData(
      pkt, cmdLen + 2, resp, respLen, nullptr, 0, true);
  return s == MFRC522::STATUS_OK;
}

// ──────────────────────────────────────────────────────────────
//  Gen3 ("APDU") magic card support
//  Gen3 cards accept an ISO7816-style APDU (90 F0 CC CC 10 <block0>)
//  after normal anticollision/select to write block 0, including the
//  UID.  No MIFARE auth required.  All other blocks use standard auth.
//  Detection is implicit — a non-Gen3 card will not return 90 00.
// ──────────────────────────────────────────────────────────────

/* Write block 0 on a Gen3 (APDU) card; also serves as detection.
   Command: CLA=90 INS=F0 P1=CC P2=CC Lc=10 <16-byte block 0>
   Returns true only if the card responds with status bytes 90 00. */
static bool gen3WriteBlock0(const uint8_t* block0) {
  uint8_t cmd[21];
  cmd[0] = 0x90; cmd[1] = 0xF0; cmd[2] = 0xCC; cmd[3] = 0xCC; cmd[4] = 0x10;
  memcpy(cmd + 5, block0, 16);
  uint8_t resp[8]; uint8_t respLen = sizeof(resp);
  if (!rfidRawCmd(cmd, 21, resp, &respLen)) return false;
  // Expected: 90 00  (2 status bytes; CRC stripped by checkCRC=true)
  return respLen >= 2 && resp[0] == 0x90 && resp[1] == 0x00;
}

// ──────────────────────────────────────────────────────────────
//  Gen4 (GTU / GDM / USCUID "CF-command") magic card support
//  Protocol:  CF <password[4]> <cmd> [data]
//  Default password: 00 00 00 00
//    CC               – version probe  (response: 00 00 00 02 AA)
//    CD <blk> <16b>   – backdoor write any block, incl. block 0 / UID
// ──────────────────────────────────────────────────────────────

static const uint8_t GEN4_PW[4] = { 0x00, 0x00, 0x00, 0x00 };

/* Probe for a Gen4 card by sending version command CF <pw> CC.
   Genuine Gen4 response: 00 00 00 02 AA.
   Returns true if Gen4 detected. */
static bool gen4Detect() {
  uint8_t cmd[6];
  cmd[0] = 0xCF;  memcpy(cmd + 1, GEN4_PW, 4);  cmd[5] = 0xCC;
  uint8_t resp[10]; uint8_t respLen = sizeof(resp);
  if (!rfidRawCmd(cmd, 6, resp, &respLen)) return false;
  return respLen >= 5 &&
         resp[0] == 0x00 && resp[1] == 0x00 && resp[2] == 0x00 &&
         resp[3] == 0x02 && resp[4] == 0xAA;
}

/* Write one 16-byte block on a Gen4 card via CF <pw> CD <block> <data>.
   Gen4 ACK is a raw 4-bit MIFARE ACK (0x0A); CRC check may fail on it,
   so we first try with CRC-checked response then retry without CRC check. */
static bool gen4WriteBlock(uint8_t blockAddr, const uint8_t* data16) {
  uint8_t cmd[23];
  cmd[0] = 0xCF;  memcpy(cmd + 1, GEN4_PW, 4);
  cmd[5] = 0xCD;  cmd[6] = blockAddr;
  memcpy(cmd + 7, data16, 16);

  uint8_t resp[8]; uint8_t respLen = sizeof(resp);
  if (rfidRawCmd(cmd, 23, resp, &respLen)) return true;  // CRC-checked path OK

  // Retry without response CRC check — card may send raw 4-bit ACK (0x0A)
  uint8_t pkt[25];
  memcpy(pkt, cmd, 23);
  byte crc[2];
  if (rfid.PCD_CalculateCRC(pkt, 23, crc) != MFRC522::STATUS_OK) return false;
  pkt[23] = crc[0]; pkt[24] = crc[1];
  uint8_t respLen2 = sizeof(resp); uint8_t vBits = 0;
  MFRC522::StatusCode s = rfid.PCD_TransceiveData(
      pkt, 25, resp, &respLen2, &vBits, 0, false);
  return s == MFRC522::STATUS_OK && (resp[0] & 0x0F) == 0x0A;
}

/* Read the 20-byte GTU config block from a Gen4 card (CF <pw> C6).
   Returns true and fills cfg[20] on success. */
static bool gen4ReadConfig(uint8_t cfg[20]) {
  uint8_t cmd[6];
  cmd[0] = 0xCF;  memcpy(cmd + 1, GEN4_PW, 4);  cmd[5] = 0xC6;
  uint8_t resp[24]; uint8_t respLen = sizeof(resp);
  if (!rfidRawCmd(cmd, 6, resp, &respLen)) return false;
  if (respLen < 20) return false;
  memcpy(cfg, resp, 20);
  return true;
}

/* Write the 20-byte GTU config block to a Gen4 card (CF <pw> F0 <cfg>).
   Returns true on success. */
static bool gen4WriteConfig(const uint8_t cfg[20]) {
  uint8_t cmd[26];
  cmd[0] = 0xCF;  memcpy(cmd + 1, GEN4_PW, 4);  cmd[5] = 0xF0;
  memcpy(cmd + 6, cfg, 20);
  uint8_t resp[8]; uint8_t respLen = sizeof(resp);
  // Try CRC-checked response first
  if (rfidRawCmd(cmd, 26, resp, &respLen)) return true;
  // Retry without response CRC (card may ACK with raw 4-bit nibble)
  uint8_t pkt[28];
  memcpy(pkt, cmd, 26);
  byte crc[2];
  if (rfid.PCD_CalculateCRC(pkt, 26, crc) != MFRC522::STATUS_OK) return false;
  pkt[26] = crc[0]; pkt[27] = crc[1];
  uint8_t respLen2 = sizeof(resp); uint8_t vBits = 0;
  MFRC522::StatusCode s = rfid.PCD_TransceiveData(
      pkt, 28, resp, &respLen2, &vBits, 0, false);
  return s == MFRC522::STATUS_OK;
}

/* Seal a Gen4 card by setting GTU mode byte (cfg[0]) to 0x00.
   Mode 0x00 = magic-mode permanently disabled; card behaves as standard MIFARE.
   Reads current config, patches byte 0, writes back.
   Returns true on success. */
static bool gen4Seal() {
  uint8_t cfg[20] = {};
  if (!gen4ReadConfig(cfg)) {
    DBGLN("[WRITE] Gen4 seal: config read failed");
    return false;
  }
  DBGF("[WRITE] Gen4 cfg[0] before seal: 0x%02X\n", cfg[0]);
  if (cfg[0] == 0x00) {
    DBGLN("[WRITE] Gen4 already sealed (mode=0x00)");
    return true;  // already sealed
  }
  cfg[0] = 0x00;  // disable magic mode permanently
  bool ok = gen4WriteConfig(cfg);
  DBGF("[WRITE] Gen4 seal write: %s\n", ok ? "OK" : "FAIL");
  return ok;
}

/* Unlock a previously sealed Gen4 card by restoring cfg[0] = 0x03 (magic always on).
   Returns true on success. */
static bool gen4Unlock() {
  uint8_t cfg[20] = {};
  if (!gen4ReadConfig(cfg)) {
    DBGLN("[WRITE] Gen4 unlock: config read failed (card may be sealed)");
    return false;
  }
  DBGF("[WRITE] Gen4 cfg[0] before unlock: 0x%02X\n", cfg[0]);
  if (cfg[0] == 0x03) {
    DBGLN("[WRITE] Gen4 already unlocked (mode=0x03)");
    return true;  // already in magic-always-on mode
  }
  cfg[0] = 0x03;  // magic mode always enabled
  bool ok = gen4WriteConfig(cfg);
  DBGF("[WRITE] Gen4 unlock write: %s\n", ok ? "OK" : "FAIL");
  return ok;
}

/* Read the Gen4 GTU mode byte (cfg[0]).
   Returns the mode byte, or 0xFF on failure.
   Mode values:
     0x00 = magic permanently disabled (sealed)
     0x01 = magic disabled until power cycle
     0x02 = shadow mode
     0x03 = magic always on (factory default) */
static uint8_t gen4GetMode() {
  uint8_t cfg[20] = {};
  if (!gen4ReadConfig(cfg)) return 0xFF;
  return cfg[0];
}

/* Touch action menu for Gen4 Seal/Unlock/Skip.
   sel: 0=Skip, 1=Seal (0x00), 2=Unlock (0x03) */
static void drawGen4ActionMenu(int sel, const char* header, const char* info) {
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader(header);
  int startY = 118;
  if (info && info[0]) {
    lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK); lcd.setTextSize(3);
    lcd.setCursor(12, 130); lcd.print(info);
    startY = 164;
  }
  static const char* opts[3] = { "Skip", "Seal", "Unlock" };
  const int bw = 350, bh = 80, gap = 16;
  lcd.setTextSize(3);
  for (int i = 0; i < 3; i++) {
    int x = (LCD_WIDTH - bw) / 2;
    int y = startY + i * (bh + gap);
    drawBtn(x, y, bw, bh, TFT_DARKGREY, opts[i]);
  }
  lcd.setCursor(LCD_WIDTH / 2 - 70, startY + 3 * (bh + gap) + 10);
  lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  lcd.setTextSize(2);
  lcd.print("Tap screen to continue");
  drawFooter(); lcd.display();
}

// Flash a colour n times with 120 ms on / 120 ms off, then restore to `restoreR/G/B`
static void ledFlash(uint8_t r, uint8_t g, uint8_t b,
                      uint8_t flashes) {
  (void)r; (void)g; (void)b; (void)flashes;
}

// ──────────────────────────────────────────────────────────────
//  Gen2 (CUID/FUID) block-0 access-bit lock / unlock
// ──────────────────────────────────────────────────────────────
/* MIFARE Classic access-bit layout in the sector trailer (bytes 6-8):
   Byte 6: ~C2[3] ~C2[2] ~C2[1] ~C2[0] ~C1[3] ~C1[2] ~C1[1] ~C1[0]
   Byte 7:  C1[3]  C1[2]  C1[1]  C1[0] ~C3[3] ~C3[2] ~C3[1] ~C3[0]
   Byte 8:  C3[3]  C3[2]  C3[1]  C3[0]  C2[3]  C2[2]  C2[1]  C2[0]
   ab[0..2] maps to bytes 6-8 of the trailer block.
   blkInSec = 0..3 (0-2 = data blocks, 3 = trailer itself).

   Useful data-block access conditions:
     AC 000 (C1=0,C2=0,C3=0): R/W with Key A or B — default / unlocked
     AC 010 (C1=0,C2=1,C3=0): Read A|B, Write never — locked (read-only)  */

/* Decode the 3-bit access condition for block blkInSec (0-3). */
static uint8_t mfGetAC(const uint8_t ab[3], uint8_t blkInSec) {
  uint8_t c1 = (ab[1] >> (4 + blkInSec)) & 1;  // byte7 bit(4+n)
  uint8_t c2 = (ab[2] >>       blkInSec)  & 1;  // byte8 bit(n)
  uint8_t c3 = (ab[2] >> (4 + blkInSec)) & 1;  // byte8 bit(4+n)
  return (c3 << 2) | (c2 << 1) | c1;
}

/* Encode a 3-bit access condition (b0=C1,b1=C2,b2=C3) into ab[0..2]. */
static void mfSetAC(uint8_t ab[3], uint8_t blkInSec, uint8_t ac) {
  uint8_t c1 = (ac >> 0) & 1, c2 = (ac >> 1) & 1, c3 = (ac >> 2) & 1;
  uint8_t bit  = 1u <<      blkInSec;
  uint8_t bit4 = 1u << (4 + blkInSec);
  // byte 6: ~C2[n] at bit4, ~C1[n] at bit
  if (!c2) ab[0] |= bit4; else ab[0] &= (uint8_t)~bit4;
  if (!c1) ab[0] |= bit;  else ab[0] &= (uint8_t)~bit;
  // byte 7: C1[n] at bit4, ~C3[n] at bit
  if ( c1) ab[1] |= bit4; else ab[1] &= (uint8_t)~bit4;
  if (!c3) ab[1] |= bit;  else ab[1] &= (uint8_t)~bit;
  // byte 8: C3[n] at bit4, C2[n] at bit
  if ( c3) ab[2] |= bit4; else ab[2] &= (uint8_t)~bit4;
  if ( c2) ab[2] |= bit;  else ab[2] &= (uint8_t)~bit;
}

/* Return true if block 0 is writable (confirms Gen2 magic mode).
   Card must already be selected. Authenticates sector 0 and attempts
   writing the same block-0 bytes back; succeeds only on Gen2 cards. */
static bool gen2ProbeWritable(const uint8_t uid[4]) {
  uint8_t kA[16][6], kB[16][6];
  bambuDeriveKeys(uid, kA, kB);
  MFRC522::MIFARE_Key mA, mB, mDef;
  memcpy(mA.keyByte, kA[0], 6);
  memcpy(mB.keyByte, kB[0], 6);
  memset(mDef.keyByte, 0xFF, 6);

  bool authed = tryAuth(3, &mB, false) || tryAuth(3, &mA, true)
             || tryAuth(3, &mDef, false) || tryAuth(3, &mDef, true);
  if (!authed) { DBGLN("[GEN2] probe: auth fail"); return false; }

  uint8_t blk[18]; uint8_t sz = 18;
  if (rfid.MIFARE_Read(0, blk, &sz) != MFRC522::STATUS_OK) {
    DBGLN("[GEN2] probe: read block 0 fail"); return false;
  }
  MFRC522::StatusCode ws = rfid.MIFARE_Write(0, blk, 16);
  DBGF("[GEN2] probe block 0 write: %s\n",
       ws == MFRC522::STATUS_OK ? "Gen2 OK" : "standard MIFARE (locked)");
  return ws == MFRC522::STATUS_OK;
}

/* Lock block 0 on a Gen2 card: sets sector-0 block-0 AC to 010 (read-only).
   Single auth session: read trailer, modify AC bits, write trailer, verify.
   Retry path: halt -> re-select -> re-auth -> write if first write fails. */
static bool gen2LockBlock0(const uint8_t uid[4]) {
  uint8_t kA[16][6], kB[16][6];
  bambuDeriveKeys(uid, kA, kB);
  MFRC522::MIFARE_Key mA, mB, mDef;
  memcpy(mA.keyByte, kA[0], 6);
  memcpy(mB.keyByte, kB[0], 6);
  memset(mDef.keyByte, 0xFF, 6);

  // ── Auth once (Key B first – needed to write AC bits in most configs) ──
  bool authed = tryAuth(3, &mB, false) || tryAuth(3, &mA, true)
             || tryAuth(3, &mDef, false) || tryAuth(3, &mDef, true);
  if (!authed) { DBGLN("[GEN2] lock: auth fail"); return false; }

  uint8_t trailer[18]; uint8_t sz = 18;
  if (rfid.MIFARE_Read(3, trailer, &sz) != MFRC522::STATUS_OK) {
    DBGLN("[GEN2] lock: read trailer fail"); return false;
  }

  uint8_t ab[3] = { trailer[6], trailer[7], trailer[8] };
  uint8_t curAC = mfGetAC(ab, 0);
  DBGF("[GEN2] lock: block 0 AC before=%d\n", curAC);
  if (curAC == 0b010) { DBGLN("[GEN2] block 0 already locked"); return true; }

  mfSetAC(ab, 0, 0b010);  // read-only
  trailer[6] = ab[0]; trailer[7] = ab[1]; trailer[8] = ab[2];

  // ── Write trailer in the SAME auth session (no StopCrypto / re-auth) ──
  MFRC522::StatusCode ws = rfid.MIFARE_Write(3, trailer, 16);
  DBGF("[GEN2] lock trailer write (1st): %s\n", ws == MFRC522::STATUS_OK ? "OK" : "FAIL");

  // ── Retry: halt -> re-select -> re-auth -> write ───────────────────────
  if (ws != MFRC522::STATUS_OK) {
    rfid.PCD_StopCrypto1();
    rfid.PICC_HaltA();
    if (!rfidReSelect()) { DBGLN("[GEN2] lock: retry re-select fail"); return false; }
    authed = tryAuth(3, &mB, false) || tryAuth(3, &mA, true)
          || tryAuth(3, &mDef, false) || tryAuth(3, &mDef, true);
    if (!authed) { DBGLN("[GEN2] lock: retry auth fail"); return false; }
    ws = rfid.MIFARE_Write(3, trailer, 16);
    DBGF("[GEN2] lock trailer write (retry): %s\n", ws == MFRC522::STATUS_OK ? "OK" : "FAIL");
    if (ws != MFRC522::STATUS_OK) return false;
  }

  // ── Verify: re-read trailer and confirm AC actually changed ────────────
  uint8_t verify[18]; uint8_t vsz = 18;
  if (rfid.MIFARE_Read(3, verify, &vsz) != MFRC522::STATUS_OK) {
    DBGLN("[GEN2] lock: verify read fail"); return false;
  }
  uint8_t vab[3] = { verify[6], verify[7], verify[8] };
  uint8_t newAC = mfGetAC(vab, 0);
  DBGF("[GEN2] lock verify: block 0 AC after=%d\n", newAC);
  if (newAC != 0b010) {
    DBGLN("[GEN2] lock: AC not changed — trailer write was silently rejected");
    return false;
  }
  return true;
}

/* Unlock block 0 on a Gen2 card: restores AC to 000 (read-write).
   Single auth session: read trailer, modify AC bits, write trailer, verify.
   Retry path: halt -> re-select -> re-auth -> write if first write fails. */
static bool gen2UnlockBlock0(const uint8_t uid[4]) {
  uint8_t kA[16][6], kB[16][6];
  bambuDeriveKeys(uid, kA, kB);
  MFRC522::MIFARE_Key mA, mB, mDef;
  memcpy(mA.keyByte, kA[0], 6);
  memcpy(mB.keyByte, kB[0], 6);
  memset(mDef.keyByte, 0xFF, 6);

  // ── Auth once (Key B first – needed to write AC bits in most configs) ──
  bool authed = tryAuth(3, &mB, false) || tryAuth(3, &mA, true)
             || tryAuth(3, &mDef, false) || tryAuth(3, &mDef, true);
  if (!authed) { DBGLN("[GEN2] unlock: auth fail"); return false; }

  uint8_t trailer[18]; uint8_t sz = 18;
  if (rfid.MIFARE_Read(3, trailer, &sz) != MFRC522::STATUS_OK) {
    DBGLN("[GEN2] unlock: read trailer fail"); return false;
  }

  uint8_t ab[3] = { trailer[6], trailer[7], trailer[8] };
  uint8_t curAC = mfGetAC(ab, 0);
  DBGF("[GEN2] unlock: block 0 AC before=%d\n", curAC);
  if (curAC == 0b000) { DBGLN("[GEN2] block 0 already unlocked"); return true; }

  mfSetAC(ab, 0, 0b000);  // fully accessible
  trailer[6] = ab[0]; trailer[7] = ab[1]; trailer[8] = ab[2];

  // ── Write trailer in the SAME auth session (no StopCrypto / re-auth) ──
  MFRC522::StatusCode ws = rfid.MIFARE_Write(3, trailer, 16);
  DBGF("[GEN2] unlock trailer write (1st): %s\n", ws == MFRC522::STATUS_OK ? "OK" : "FAIL");

  // ── Retry: halt -> re-select -> re-auth -> write ───────────────────────
  if (ws != MFRC522::STATUS_OK) {
    rfid.PCD_StopCrypto1();
    rfid.PICC_HaltA();
    if (!rfidReSelect()) { DBGLN("[GEN2] unlock: retry re-select fail"); return false; }
    authed = tryAuth(3, &mB, false) || tryAuth(3, &mA, true)
          || tryAuth(3, &mDef, false) || tryAuth(3, &mDef, true);
    if (!authed) { DBGLN("[GEN2] unlock: retry auth fail"); return false; }
    ws = rfid.MIFARE_Write(3, trailer, 16);
    DBGF("[GEN2] unlock trailer write (retry): %s\n", ws == MFRC522::STATUS_OK ? "OK" : "FAIL");
    if (ws != MFRC522::STATUS_OK) return false;
  }

  // ── Verify: re-read trailer and confirm AC actually changed ────────────
  uint8_t verify[18]; uint8_t vsz = 18;
  if (rfid.MIFARE_Read(3, verify, &vsz) != MFRC522::STATUS_OK) {
    DBGLN("[GEN2] unlock: verify read fail"); return false;
  }
  uint8_t vab[3] = { verify[6], verify[7], verify[8] };
  uint8_t newAC = mfGetAC(vab, 0);
  DBGF("[GEN2] unlock verify: block 0 AC after=%d\n", newAC);
  if (newAC != 0b000) {
    DBGLN("[GEN2] unlock: AC not changed — trailer write was silently rejected");
    return false;
  }
  return true;
}

/* Force a full trailer rewrite on a Gen2 card.
   Reads the card UID, derives Bambu keys from it, and writes sector 0 trailer
   with those keys + AC=000 (fully unlocked).  Tries:
     A. Immediate write after fresh auth (no prior I/O)
     B. Key A write after auth with Key A
     C. Key B write with full halt/re-select/retry loop
     D. Raw transceive fallback
   Verifies by re-reading AC bits. */
static bool gen2RepairTag(const uint8_t uid[4]) {
  uint8_t kA[16][6], kB[16][6];
  bambuDeriveKeys(uid, kA, kB);
  MFRC522::MIFARE_Key mA, mB, mDef, mDefB;
  memcpy(mA.keyByte, kA[0], 6);
  memcpy(mB.keyByte, kB[0], 6);
  memset(mDef.keyByte,  0xFF, 6);
  memset(mDefB.keyByte, 0xFF, 6);

  // Build trailer: UID-derived keys + AC=000 for all blocks
  uint8_t trl[16];
  memcpy(trl,      kA[0], 6);
  memcpy(trl + 10, kB[0], 6);
  {
    uint8_t ab[3] = { 0xFF, 0x07, 0x80 };
    for (int b = 0; b < 4; b++) mfSetAC(ab, b, 0b000);
    trl[6] = ab[0]; trl[7] = ab[1]; trl[8] = ab[2];
  }

  // ── Helper: write trailer after fresh auth (no read in between) ──
  auto tryWrite = [&](bool useKeyB, const char* label) -> bool {
    MFRC522::StatusCode ws = rfid.MIFARE_Write(3, trl, 16);
    DBGF("[GEN2] repair %s write: %s\n", label,
         ws == MFRC522::STATUS_OK ? "OK" : "FAIL");
    if (ws == MFRC522::STATUS_OK) {
      // Verify: re-auth and re-read
      rfid.PCD_StopCrypto1(); rfid.PICC_HaltA();
      if (rfidReSelect() && (tryAuth(3, &mB, false) || tryAuth(3, &mA, true))) {
        uint8_t v[18]; uint8_t vsz = 18;
        if (rfid.MIFARE_Read(3, v, &vsz) == MFRC522::STATUS_OK) {
          uint8_t vab[3] = { v[6], v[7], v[8] };
          if (mfGetAC(vab, 0) == 0 && mfGetAC(vab, 1) == 0 && mfGetAC(vab, 2) == 0)
            return true;
        }
      }
    }
    return false;
  };

  // ── Attempt A: immediate write after fresh auth (no prior I/O) ──
  rfid.PCD_StopCrypto1(); rfid.PICC_HaltA();
  if (rfidReSelect()) {
    if (tryAuth(3, &mB, false) || tryAuth(3, &mDefB, false)) {
      if (tryWrite(true, "immediate B")) return true;
    }
    rfid.PCD_StopCrypto1();
    if (rfidReSelect() && (tryAuth(3, &mA, true) || tryAuth(3, &mDef, true))) {
      if (tryWrite(false, "immediate A")) return true;
    }
  }

  // ── Attempt B: Key A write (fast path for cards with permissive AC) ──
  rfid.PCD_StopCrypto1(); rfid.PICC_HaltA();
  if (rfidReSelect() && (tryAuth(3, &mA, true) || tryAuth(3, &mDef, true))) {
    if (tryWrite(false, "key A")) return true;
  }

  // ── Attempt C: Key B write with halt/re-select/retry loop ──
  for (int attempt = 0; attempt < 3; attempt++) {
    rfid.PCD_StopCrypto1(); rfid.PICC_HaltA();
    if (!rfidReSelect()) break;
    if ((tryAuth(3, &mB, false) || tryAuth(3, &mDefB, false)) && tryWrite(true, "key B"))
      return true;
  }

  // ── Attempt D: raw transceive as last resort ──
  rfid.PCD_StopCrypto1(); rfid.PICC_HaltA();
  if (rfidReSelect() && (tryAuth(3, &mB, false) || tryAuth(3, &mA, true))) {
    byte cmd[2] = { 0xA0, 0x03 };
    byte resp[4]; byte rlen = sizeof(resp);
    auto st = rfid.PCD_TransceiveData(cmd, 2, resp, &rlen, nullptr, 0, true);
    if (st == MFRC522::STATUS_OK && rlen >= 1 && resp[0] == 0x0A) {
      rlen = sizeof(resp);
      st = rfid.PCD_TransceiveData((byte*)trl, 16, resp, &rlen, nullptr, 0, true);
      if (st == MFRC522::STATUS_OK && rlen >= 1 && resp[0] == 0x0A) {
        DBGLN("[GEN2] repair: raw transceive OK");
        // Raw write gives no verification — assume it worked
        return true;
      }
    }
  }

  DBGLN("[GEN2] repair: all attempts FAILED");
  return false;
}

/* Touch action menu for Gen2 lock/unlock/repair.
   sel: 0=Skip  1=Repair Tag  2=Lock Block 0  3=Unlock Block 0 */
static void drawGen2ActionMenu(int sel, const char* header, const char* info) {
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader(header);
  int startY = 114;
  if (info && info[0]) {
    lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK); lcd.setTextSize(3);
    lcd.setCursor(12, 130); lcd.print(info);
    startY = 162;
  }
  static const char* OPTS[] = { "Skip", "Repair Tag", "Lock Block 0", "Unlock Block 0" };
  const int bw = 350, bh = 60, gap = 6;
  lcd.setTextSize(3);
  for (int i = 0; i < 4; i++) {
    int x = (LCD_WIDTH - bw) / 2;
    int y = startY + i * (bh + gap);
    drawBtn(x, y, bw, bh, TFT_DARKGREY, OPTS[i]);
  }
  drawFooter(); lcd.display();
}

/* Write a 1024-byte dump to a card.
   Card-type detection order:
     1. Gen1A  – responds to 0x40/0x43 backdoor; all 64 blocks written verbatim
                 (block 0 / UID overwritten; trailer keys verbatim from dump).
     2. Gen4   – GTU / GDM / USCUID; responds to CF 00000000 CC version probe;
                 all 64 blocks written verbatim via CF <pw> CD backdoor commands.
     3. Gen3   – APDU-based; block 0 written via 90 F0 CC CC 10 APDU;
                 blocks 1-63 via 3-key auth; trailers use dump-UID-derived keys.
     4. Gen2   – standard MIFARE (CUID/FUID); block 0 writable after normal auth;
                 detected implicitly during sector 0 write; trailers re-keyed.
     5. Normal MIFARE – block 0 is hardware-locked; written with 3-key strategy,
                 trailer keys rewritten using dest-UID-derived keys.

   3-key normal-auth priority per sector:
     1. 0xFF…FF  (factory-blank card)
     2. Key derived from the DESTINATION card's own UID  (previously Bambu-keyed)
     3. Key A embedded in the dump  (source-UID key, last resort)

   Trailer blocks are written with keys derived from the DESTINATION card UID
   so the Bambu printer can authenticate the tag correctly after writing.

   Returns the number of sectors successfully written (0 = total failure). */
int rfidWriteDump(const uint8_t* buf, bool /*isMagicCard — now auto-detected via Gen1A*/) {
  // ── Card select ──────────────────────────────────────────────────────────
  if (!rfid.PICC_IsNewCardPresent()) { delay(18); }
  if (!rfid.PICC_ReadCardSerial())   { delay(18); }

  if (!rfid.PICC_IsNewCardPresent()) {
    DBGLN("[WRITE] PICC_IsNewCardPresent FAIL");
    return 0;
  }
  DBGLN("[WRITE] PICC_IsNewCardPresent OK");

  if (!rfid.PICC_ReadCardSerial()) {
    DBGLN("[WRITE] PICC_ReadCardSerial FAIL");
    return 0;
  }
  DBGLN("[WRITE] PICC_ReadCardSerial OK");

  // ── Read destination UID ─────────────────────────────────────────────────
  uint8_t destUID[4];
  memcpy(destUID, rfid.uid.uidByte, 4);
  DBGF("[WRITE] Dest UID: %02X %02X %02X %02X\n",
       destUID[0], destUID[1], destUID[2], destUID[3]);

  // ── Auto-detect card type ────────────────────────────────────────────────────────
  //  Detection order: Gen1A → Gen4 → Gen3 → Gen2 (implicit) → standard MIFARE
  bool isGen1A = gen1aUnlock();
  if (isGen1A) {
    DBGLN("[WRITE] Gen1A magic card detected — bypassing auth (backdoor write)");
  } else {
    // Re-select after Gen1A probe: the 0x40/0x43 commands may have confused the card
    // into HALT or IDLE state, which makes all subsequent auth attempts fail even
    // with correct keys. Antenna power-cycle forces the card back to IDLE.
    DBGLN("[WRITE] Gen1A probe failed — re-selecting card");
    if (!rfidReSelect()) {
      DBGLN("[WRITE] Re-select after Gen1A — card removed");
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      return 0;
    }
  }

  bool isGen4    = false;  // GTU / GDM / USCUID CF-command cards
  bool isGen3    = false;  // APDU block-0-writable cards
  bool isGen2    = false;  // detected implicitly during sector 0 normal-auth write
  int  sectorsOk = 0;
  bool reAssemble = false;

  // ── Gen1A path: write all 64 blocks verbatim ─────────────────────────────
  //  Block 0 (UID) written; trailer keys kept verbatim from dump.
  if (isGen1A) {
    for (int sec = 0; sec < NUM_SECTORS; sec++) {
      bool sectorOk = true;
      for (int b = 0; b < BLOCKS_PER_SECTOR; b++) {
        int addr = sec * BLOCKS_PER_SECTOR + b;
        bool ok = gen1aWriteBlock(addr, buf + addr * BYTES_PER_BLOCK);
        DBGF("[WRITE]   blk %02d -> %s\n", addr, ok ? "OK" : "FAIL");
        if (!ok) sectorOk = false;
      }
      DBGF("[WRITE] sector %02d -> %s\n", sec, sectorOk ? "OK" : "FAIL");
      if (sectorOk) sectorsOk++;
      if (g_writeSectorCb) g_writeSectorCb(sec + 1, NUM_SECTORS);
    }

  // ── Gen4 / Gen3 / Gen2 / Normal path ──────────────────────────────────────
  } else {

    // ── Gen4 probe (CF 00000000 CC version command) ─────────────────────
    //  Non-Gen4 MIFARE cards may be confused by the CF command → re-select.
    isGen4 = gen4Detect();
    if (isGen4) {
      DBGLN("[WRITE] Gen4 (GTU/GDM/USCUID) magic card detected — CF backdoor write");
    } else {
      rfidReSelect();  // restore ACTIVE state after failed Gen4 probe
    }

    if (isGen4) {
      // ── Gen4 path: write all 64 blocks via CF CD commands ────────────────
      for (int sec = 0; sec < NUM_SECTORS; sec++) {
        bool sectorOk = true;
        for (int b = 0; b < BLOCKS_PER_SECTOR; b++) {
          int addr = sec * BLOCKS_PER_SECTOR + b;
          bool ok = gen4WriteBlock(addr, buf + addr * BYTES_PER_BLOCK);
          DBGF("[WRITE]   blk %02d -> %s\n", addr, ok ? "OK" : "FAIL");
          if (!ok) sectorOk = false;
        }
        DBGF("[WRITE] sector %02d -> %s\n", sec, sectorOk ? "OK" : "FAIL");
        if (sectorOk) sectorsOk++;
        if (g_writeSectorCb) g_writeSectorCb(sec + 1, NUM_SECTORS);
      }

      // ── Gen4 seal/unlock prompt (only if all sectors written successfully) ─
      if (sectorsOk == NUM_SECTORS) {
        // 3-option mini-menu: 0=Skip, 1=Seal, 2=Unlock
        int g4sel = 0;
        drawGen4ActionMenu(g4sel, "Write Tag", "");
        unsigned long t0 = millis();
        bool confirmed = false;
        while (millis() - t0 < 20000) {
          httpServer.handleClient();
          int tx, ty;
          if (touchGet(&tx, &ty)) {
            int bw = 350, bh = 80, gap = 16, startY = 80;
            int bx = (LCD_WIDTH - bw) / 2;
            for (int i = 0; i < 3; i++) {
              int by = startY + i * (bh + gap);
              if (tx >= bx && tx <= bx + bw && ty >= by && ty <= by + bh)
                { g4sel = i; confirmed = true; break; }
            }
            if (confirmed) break;
          }
          delay(10);
        }
        if (confirmed && g4sel != 0) {
          if (g4sel == 1) {
            showStatus("Write Tag\nGen4\n\nSealing...");
            bool ok = gen4Seal();
            DBGF("[WRITE] Gen4 seal: %s\n", ok ? "OK" : "FAIL");
            if (ok) { showStatus("Write Tag\nGen4 Sealed!\nMagic mode OFF\nStandard MIFARE"); ledFlash(0, 255, 0, 2); }
            else    { showStatus("Write Tag\nGen4 Seal FAIL"); ledFlash(255, 0, 0, 2); }
          } else {  // g4sel == 2
            showStatus("Write Tag\nGen4\n\nUnlocking...");
            bool ok = gen4Unlock();
            DBGF("[WRITE] Gen4 unlock: %s\n", ok ? "OK" : "FAIL");
            if (ok) { showStatus("Write Tag\nGen4 Unlocked!\nMagic mode ON"); ledFlash(0, 255, 0, 2); }
            else    { showStatus("Write Tag\nGen4 Unlock FAIL"); ledFlash(255, 0, 0, 2); }
          }
          t0 = millis();
          while (millis() - t0 < 8000) {
            httpServer.handleClient();
            if (touchPoll()) break;
            delay(10);
          }
        } else {
          DBGLN("[WRITE] Gen4 action skipped");
        }
      }

    } else {
      // ── Gen3 probe: APDU 90 F0 CC CC 10 <block0> ────────────────────────
      //  Detection and block-0 write are the same operation; non-Gen3 cards
      //  will not respond 90 00, and may need re-select afterwards.
      isGen3 = gen3WriteBlock0(buf);  // buf[0..15] = block 0 data
      if (isGen3) {
        DBGLN("[WRITE] Gen3 (APDU) magic card detected — block 0 (UID) written via APDU");
      }
      // Re-select always: Gen3 success changes UID; failure may confuse card
      if (!rfidReSelect()) {
        DBGLN("[WRITE] Re-select failed after Gen3 probe — card removed?");
        return 0;
      }

      // Derive trailer keys from the EFFECTIVE card UID:
      //   Gen3: dump UID is now the card's UID   → use buf[0..3]
      //   Gen2: UID changes on first block-0 write → re-derived inline below
      //   Standard: card UID unchanged             → use original destUID
      uint8_t effectiveUID[4];
      memcpy(effectiveUID, isGen3 ? buf : destUID, 4);

      uint8_t keysDestA[16][6], keysDestB[16][6];
      bambuDeriveKeys(effectiveUID, keysDestA, keysDestB);
      DBGLN("[WRITE] Destination key derivation complete.");

      // keysOrigA/B: Bambu-derived keys from the ORIGINAL card UID (destUID).
      // For Gen2 overwrite: after block-0 write changes the UID, keysDestA/B
      // are re-derived from the dump UID. keysOrigA/B retains the OLD UID keys
      // so that sectors 1-15 on a previously-Bambu-written card can be authed.
      uint8_t keysOrigA[16][6], keysOrigB[16][6];
      bambuDeriveKeys(destUID, keysOrigA, keysOrigB);

      MFRC522::MIFARE_Key kDef;
      memset(kDef.keyByte, 0xFF, 6);

      // ── Pre-loop Gen2 probe (sector 0, factory keys only) ────────────
      // Fresh Gen2 magic cards have factory keys (0xFF×6).  If we can auth
      // with factory Key B and write block 0, the card is Gen2 — do the UID
      // swap now so the trailer write below uses correct keys for the new UID.
      // Re-used Gen2 cards (trailer already written) will fail here (old UID
      // keys differ from factory) and fall through to the sector 0 loop.
      {
        MFRC522::MIFARE_Key probekDefB;
        memset(probekDefB.keyByte, 0xFF, 6);
        if (tryAuth(3, &probekDefB, false)) {
          MFRC522::StatusCode ws = rfid.MIFARE_Write(0, (uint8_t*)(buf), BYTES_PER_BLOCK);
          rfid.PCD_StopCrypto1();
          if (ws == MFRC522::STATUS_OK) {
            DBGLN("[WRITE] Gen2 detected — factory auth + block 0 write OK");
            isGen2 = true;
            bambuDeriveKeys(buf, keysDestA, keysDestB);
            if (!rfidReSelect()) {
              DBGLN("[WRITE] Gen2 re-select: card lost, continuing");
            }
          }
        } else {
          DBGLN("[WRITE] Gen2 probe: factory auth FAIL — not fresh Gen2");
        }
      }

      for (int sec = 0; sec < NUM_SECTORS; sec++) {
        int trailerBlk = sec * BLOCKS_PER_SECTOR + 3;

        // Assemble all candidate keys for this sector
        MFRC522::MIFARE_Key kDA, kDB, kOA, kOB, kDpA, kDpB, kDefB;
        memcpy(kDA.keyByte,   keysDestA[sec], 6);                           // Key A new/dump UID
        memcpy(kDB.keyByte,   keysDestB[sec], 6);                           // Key B new/dump UID
        memcpy(kOA.keyByte,   keysOrigA[sec], 6);                           // Key A original card UID
        memcpy(kOB.keyByte,   keysOrigB[sec], 6);                           // Key B original card UID
        memcpy(kDpA.keyByte,  buf + trailerBlk * BYTES_PER_BLOCK,      6);  // Key A from dump trailer
        memcpy(kDpB.keyByte,  buf + trailerBlk * BYTES_PER_BLOCK + 10, 6);  // Key B from dump trailer
        memset(kDefB.keyByte, 0xFF, 6);

        bool sectorOk = true;

        // ── Pass 1: write trailer ───────────────────────────────────────
        // Try Key A then Key B variants.  If auth succeeds with Key A
        // but the write is rejected, the card's AC bits restrict trailer
        // writes to Key B — retry with Key B (or vice versa).
        bool trlWritten = false;
        bool trlAuthed = tryAuth(trailerBlk, &kDef, true)     // Key A 0xFF×6 (factory)
                      || tryAuth(trailerBlk, &kDefB, false)   // Key B 0xFF×6 (factory)
                      || tryAuth(trailerBlk, &kOA,  true)     // Key A original card UID
                      || tryAuth(trailerBlk, &kOB,  false)    // Key B original card UID
                      || tryAuth(trailerBlk, &kDA,  true)     // Key A new dump UID
                      || tryAuth(trailerBlk, &kDB,  false)    // Key B new dump UID
                      || tryAuth(trailerBlk, &kDpA, true)     // Key A from dump file
                      || tryAuth(trailerBlk, &kDpB, false);   // Key B from dump file

        DBGF("[WRITE] sector %02d trailer auth (A+B) -> %s\n", sec, trlAuthed ? "OK" : "FAIL");

        if (trlAuthed) {
          uint8_t trlBuf[16];
          memcpy(trlBuf,      keysDestA[sec], 6);
          memcpy(trlBuf + 6,  buf + trailerBlk * BYTES_PER_BLOCK + 6, 4);
          memcpy(trlBuf + 10, keysDestB[sec], 6);
          // Dump data blocks typically have AC=010 (read-only, Key B) while
          // the trailer block often requires Key B to write.  Unlock ALL four
          // blocks (0-3) to AC=000 (read/write with Key A|B) so the data
          // pass below works and future overwrites can use the simpler Key-A
          // auth path on Gen2 cards.
          {
            uint8_t ab[3] = { trlBuf[6], trlBuf[7], trlBuf[8] };
            for (int b = 0; b < 4; b++) mfSetAC(ab, b, 0b000);
            trlBuf[6] = ab[0]; trlBuf[7] = ab[1]; trlBuf[8] = ab[2];
          }
          MFRC522::StatusCode ts = rfid.MIFARE_Write(trailerBlk, trlBuf, BYTES_PER_BLOCK);
          DBGF("[WRITE]   trailer blk %02d (Key-A auth) -> %s\n", trailerBlk,
               ts == MFRC522::STATUS_OK ? "OK" : "FAIL");
          if (ts == MFRC522::STATUS_OK) {
            trlWritten = true;
          } else {
            // Key A lacked write permission — retry with Key B
            rfid.PCD_StopCrypto1();
            bool keyBAuth = tryAuth(trailerBlk, &kDefB, false)
                         || tryAuth(trailerBlk, &kOB,   false)
                         || tryAuth(trailerBlk, &kDB,   false)
                         || tryAuth(trailerBlk, &kDpB,  false);
            if (keyBAuth) {
              ts = rfid.MIFARE_Write(trailerBlk, trlBuf, BYTES_PER_BLOCK);
              DBGF("[WRITE]   trailer blk %02d (Key-B auth) -> %s\n", trailerBlk,
                   ts == MFRC522::STATUS_OK ? "OK" : "FAIL");
              if (ts == MFRC522::STATUS_OK) trlWritten = true;
            }
          }
          rfid.PCD_StopCrypto1();
        }
        if (!trlWritten) sectorOk = false;

        // ── Pass 2: write data blocks ────────────────────────────────────────
        // After the trailer write the card should have NEW keys → try new
        // Key B first.  But on re-used Gen2 cards the trailer write silently
        // fails and the OLD keys remain.  We detect this: if kDB fails but
        // kOB works, the card is in "legacy keys" mode and we skip the UID
        // change (block 0 write) for sector 0 to avoid UID/keys mismatch.
        bool usingDestKeys = false;  // true = new dump keys committed
        bool datAuthed = tryAuth(trailerBlk, &kDB,   false)  // Key B new dump UID
                      || tryAuth(trailerBlk, &kOB,   false)  // Key B original card UID
                      || tryAuth(trailerBlk, &kDefB, false)  // Key B 0xFF×6
                      || tryAuth(trailerBlk, &kDpB,  false)  // Key B from dump file
                      || tryAuth(trailerBlk, &kDef,  true)   // Key A 0xFF×6 fallback
                      || tryAuth(trailerBlk, &kOA,   true)   // Key A original card UID
                      || tryAuth(trailerBlk, &kDA,   true);  // Key A new dump UID

        // Detect whether the trailer commit actually changed keys.
        // If kDB works the trailer was committed (fresh Gen2 or normal MIFARE).
        // If only kOB works the Gen2 card rejected the trailer write — we skip
        // the UID change to avoid leaving the card with mismatched UID/keys.
        if (sec == 0 && !isGen2 && datAuthed) {
          rfid.PCD_StopCrypto1();
          usingDestKeys = tryAuth(trailerBlk, &kDB, false);

          if (usingDestKeys) {
            MFRC522::StatusCode ws = rfid.MIFARE_Write(0, (uint8_t*)(buf), BYTES_PER_BLOCK);
            rfid.PCD_StopCrypto1();
            if (ws == MFRC522::STATUS_OK) {
              DBGLN("[WRITE] Gen2 detected — UID changed after successful trailer");
              isGen2 = true;
              bambuDeriveKeys(buf, keysDestA, keysDestB);
              if (!rfidReSelect()) {
                DBGLN("[WRITE] Gen2 re-select: card lost, aborting");
                break;
              }
              reAssemble = true;
            }
          } else {
            DBGLN("[WRITE] Gen2 card rejects trailer write — keeping original UID");
          }

          // Re-auth: after Gen2 re-select the card has new UID; after legacy
          // skip we're still authed with kOB.  Either way, auth state is lost
          // after the stop/re-select above.
          MFRC522::MIFARE_Key *keyOrder[] = { &kDB, &kOB, &kDefB, &kDpB,
                                               &kDef, &kOA, &kDA };
          datAuthed = false;
          for (int ki = 0; ki < 7; ki++) {
            bool isKeyA = (ki >= 4);
            if (tryAuth(trailerBlk, keyOrder[ki], isKeyA)) {
              datAuthed = true; break;
            }
          }
        }

        DBGF("[WRITE] sector %02d data auth -> %s\n", sec, datAuthed ? "OK" : "FAIL");

        if (datAuthed) {
          for (int b = 0; b < 3; b++) {
            int addr = sec * BLOCKS_PER_SECTOR + b;
            if (addr == 0) {
              if (isGen2) {
                DBGLN("[WRITE]   blk 00 -> already written via Gen2, skipping");
              } else {
                DBGLN("[WRITE]   blk 00 -> read-only (standard MIFARE, skipping)");
              }
              continue;
            }
            MFRC522::StatusCode ws = rfid.MIFARE_Write(
              addr, (uint8_t*)(buf + addr * BYTES_PER_BLOCK), BYTES_PER_BLOCK);
            DBGF("[WRITE]   blk %02d -> %s\n", addr,
                 ws == MFRC522::STATUS_OK ? "OK" : "FAIL");
            if (ws != MFRC522::STATUS_OK) sectorOk = false;
          }
          rfid.PCD_StopCrypto1();
        } else {
          sectorOk = false;
        }

        if (sectorOk) sectorsOk++;
        if (g_writeSectorCb) g_writeSectorCb(sec + 1, NUM_SECTORS);

        // ── Gen2 post-sector-0 re-select ─────────────────────────────────────
        if (sec == 0 && isGen2 && reAssemble) {
          if (!rfidReSelect()) {
            DBGLN("[WRITE] Gen2 re-select after block 0 failed — aborting");
            break;
          }
          DBGLN("[WRITE] Gen2 re-select OK — reader now sees new UID");
        }
      }
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  const char* cardType = isGen1A ? "Gen1A" : isGen4 ? "Gen4"
                       : isGen3  ? "Gen3"  : isGen2  ? "Gen2" : "standard MIFARE";
  DBGF("[WRITE] %d/%d sectors written OK  [card type: %s]\n",
       sectorsOk, NUM_SECTORS, cardType);
  return sectorsOk;
}

// ──────────────────────────────────────────────────────────────
//  Touchscreen / display helpers
// ──────────────────────────────────────────────────────────────
static bool touchPoll() { int tx, ty; return touchGet(&tx, &ty); }
static bool touchCalLoad(uint16_t* params) {
  prefs.begin("touch", true);
  bool ok = prefs.getBytes("cal", params, 16) == 16;
  prefs.end();
  return ok;
}
static void touchCalSave(uint16_t* params) {
  prefs.begin("touch", false);
  prefs.putBytes("cal", params, 16);
  prefs.end();
}
static bool touchGet(int* tx, int* ty) {
  lgfx::touch_point_t tp;
  if (!lcd.getTouch(&tp, 1)) return false;
  if (tx) *tx = tp.x;
  if (ty) *ty = tp.y;
  return true;
}
static void drawBtn(int x, int y, int w, int h, uint16_t bg, const char* label) {
  lcd.fillRoundRect(x, y, w, h, 4, bg);
  lcd.setTextColor(TFT_WHITE, bg);
  lcd.setTextDatum(MC_DATUM);
  lcd.drawString(label, x + w / 2, y + h / 2);
}
static void drawWiFiIcon(int cx, int cy) {
  if (WiFi.status() != WL_CONNECTED) {
    lcd.setTextSize(3);
    lcd.setTextDatum(TL_DATUM);
    lcd.setCursor(cx - 30, cy - 12);
    lcd.print("AP");
    return;
  }
  long rssi = WiFi.RSSI();
  int bars = (rssi > -55) ? 3 : (rssi > -70) ? 2 : 1;
  uint16_t green = lcd.color565(0, 220, 0);
  uint16_t dark  = TFT_DARKGREY;

  // Bottom dot
  lcd.fillCircle(cx, cy + 14, 3, green);

  // Arc 1 – innermost
  uint16_t c1 = (bars >= 1) ? green : dark;
  lcd.fillArc(cx, cy + 8, 4, 8, 225, 315, c1);

  // Arc 2 – middle
  uint16_t c2 = (bars >= 2) ? green : dark;
  lcd.fillArc(cx, cy, 11, 15, 225, 315, c2);

  // Arc 3 – outermost
  uint16_t c3 = (bars >= 3) ? green : dark;
  lcd.fillArc(cx, cy - 8, 18, 22, 225, 315, c3);
}

static void drawStatusBar() {
  lcd.fillRect(0, 0, LCD_WIDTH, 64, TFT_NAVY);
  lcd.pushImage(4, 0, LOGO_W, LOGO_H, (uint16_t*)logoBitmap, LOGO_TRANS_KEY);
  lcd.setTextColor(TFT_WHITE, TFT_NAVY); lcd.setTextSize(5);
  lcd.setTextDatum(MC_DATUM);
  lcd.drawString("BambuTagger", LCD_WIDTH / 2, 32);
  drawWiFiIcon(LCD_WIDTH - 48, 32);
}

#define SUBHEADER_Y 64
#define SUBHEADER_H 44

static void drawSubHeader(const char* title) {
  lcd.fillRect(0, SUBHEADER_Y, LCD_WIDTH, SUBHEADER_H, TFT_DARKGREY);
  lcd.setTextColor(TFT_WHITE, TFT_DARKGREY);
  lcd.setTextSize(3);
  lcd.setTextDatum(ML_DATUM);
  lcd.drawString(title, 10, SUBHEADER_Y + SUBHEADER_H / 2);
}
#define BACK_X ((LCD_WIDTH - 200) / 2)
#define BACK_Y (LCD_HEIGHT - 24 - 56 - 5)
#define BACK_W 200
#define BACK_H 56
static void drawBackBtn() { lcd.setTextSize(2); drawBtn(BACK_X, BACK_Y, BACK_W, BACK_H, TFT_DARKGREY, "Back"); }
#define LIST_ROW_Y0 136
#define LIST_ROW_H 56
#define LIST_BTN_H 50
#define LIST_BTN_W (LCD_WIDTH - 52)
#define LIST_MAX_VIS 5

#define FOOTER_H 24

#define SB_X    (LCD_WIDTH - 34)
#define SB_W    30
#define SB_Y    LIST_ROW_Y0
#define SB_H    (LIST_MAX_VIS * LIST_ROW_H)
static void drawScrollbar(int scrollPos, int totalRows, int startY = -1, int height = -1) {
  int sy = (startY >= 0) ? startY : SB_Y;
  int sh = (height >= 0) ? height : SB_H;
  int visRows = sh / LIST_ROW_H;
  if (totalRows <= visRows) return;
  lcd.fillRoundRect(SB_X, sy, SB_W, sh, 3, TFT_DARKGREY);
  int thumbH = max(16, sh * visRows / totalRows);
  int thumbY = sy + (sh - thumbH) * scrollPos / (totalRows - visRows);
  lcd.fillRoundRect(SB_X, thumbY, SB_W, thumbH, 3, TFT_WHITE);
}

// Handle scrollbar tap: above thumb = scroll up, below thumb = scroll down
// Returns true if the tap was within the scrollbar area and handled
static bool sbTapScroll(int ttx, int tty, int totalRows, int& scrollPos, int startY, int height) {
  if (ttx < SB_X || ttx > SB_X + SB_W) return false;
  int visRows = height / LIST_ROW_H;
  if (totalRows <= visRows || tty < startY || tty > startY + height) return false;
  int thumbH = max(16, height * visRows / totalRows);
  int thumbY = startY + (height - thumbH) * scrollPos / (totalRows - visRows);
  if (tty < thumbY) {
    scrollPos = max(0, scrollPos - (visRows - 1));
  } else if (tty > thumbY + thumbH) {
    scrollPos = min(totalRows - visRows, scrollPos + (visRows - 1));
  } else return true;  // tapped on thumb, no change
  return true;
}

#define FOOTER_Y (LCD_HEIGHT - FOOTER_H)
static void drawFooter() {
  lcd.fillRect(0, FOOTER_Y, LCD_WIDTH, FOOTER_H, TFT_NAVY);
  lcd.setTextColor(TFT_WHITE, TFT_NAVY); lcd.setTextSize(2);
  lcd.setTextDatum(MC_DATUM);
  lcd.drawString("(c) 2026 by BambuTagger", LCD_WIDTH / 2, FOOTER_Y + FOOTER_H / 2);
  lcd.setTextDatum(TL_DATUM);
  lcd.setCursor(LCD_WIDTH - 75, FOOTER_Y + 4);
  lcd.print("v" FIRMWARE_VERSION);
}

void showStatus(const char* msg) {
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar();
  // Extract first line as subheader
  const char* nl = strchr(msg, '\n');
  int titleLen = nl ? min((int)(nl - msg), 31) : (int)strlen(msg);
  char title[32];
  if (titleLen) { strncpy(title, msg, titleLen); title[titleLen] = '\0'; }
  else title[0] = '\0';
  if (title[0]) drawSubHeader(title);

  // Remaining lines
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setTextSize(3); lcd.setTextWrap(true);
  int bodyLines = 0;
  for (const char* p = msg; *p; p++) if (*p == '\n') bodyLines++;
  if (nl) bodyLines--;
  int blockH = bodyLines * 36;
  int y0 = SUBHEADER_Y + SUBHEADER_H + (BACK_Y - SUBHEADER_Y - SUBHEADER_H - blockH) / 2;
  int y = y0; const char* p = nl ? nl + 1 : msg + strlen(msg); char buf[128];
  while (*p) {
    const char* nl2 = strchr(p, '\n');
    int len = nl2 ? min((int)(nl2 - p), 127) : (int)strlen(p);
    if (len) { strncpy(buf, p, len); buf[len] = '\0'; lcd.setTextDatum(MC_DATUM); lcd.drawString(buf, LCD_WIDTH / 2, y + 18); y += 36; }
    p = nl2 ? nl2 + 1 : p + len;
  }
  drawFooter(); lcd.display();
}

void showStatus2(const char* l1, const char* l2) {
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader(l1);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(3);
  int y0 = SUBHEADER_Y + SUBHEADER_H + (BACK_Y - SUBHEADER_Y - SUBHEADER_H - 36) / 2;
  lcd.setTextDatum(MC_DATUM);
  lcd.drawString(l2, LCD_WIDTH / 2, y0 + 18);
  drawFooter(); lcd.display();
}

// ──────────────────────────────────────────────────────────────
//  WS2812B LED helpers
// ──────────────────────────────────────────────────────────────

static void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  (void)r; (void)g; (void)b;
}

static void ledOff() {
}

static void ledSetTagColor(const TagInfo* t) {
  (void)t;
}

static bool ledScanPulse() {
  static uint8_t val = 0;
  static int8_t dir = 4;
  static unsigned long last = 0;
  if (millis() - last < 18) return false;
  last = millis();
  val = (uint8_t)constrain((int)val + dir, 0, 80);
  if (val == 80 || val == 0) dir = -dir;
  return (val == 0 && dir > 0);
}

// ──────────────────────────────────────────────────────────────
//  Draw main menu
// ──────────────────────────────────────────────────────────────
#define MENU_COLS 2
#define MENU_ROWS 4
#define BTN_W 360
#define BTN_H 76
#define BTN_GAP_X 24
#define BTN_GAP_Y 8
#define MENU_X0 24
#define MENU_Y0 118

void drawMenu() {
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("Menu");
  lcd.setTextSize(3);
  for (int i = 0; i < MENU_COUNT; i++) {
    int col = i / MENU_ROWS;
    int row = i % MENU_ROWS;
    int x = MENU_X0 + col * (BTN_W + BTN_GAP_X);
    int y = MENU_Y0 + row * (BTN_H + BTN_GAP_Y);
    uint16_t bg = TFT_DARKGREY;
    drawBtn(x, y, BTN_W, BTN_H, bg, MENU_ITEMS[i]);
  }
  drawFooter(); lcd.display();
}

// ──────────────────────────────────────────────────────────────
//  Draw tag info
// ──────────────────────────────────────────────────────────────
void drawTagInfo(const TagInfo* t, int) {
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("Tag Info");

  lcd.setTextSize(3);

  int y = 125;
  int c1 = 10, c2 = 230;
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setCursor(c1, y); lcd.print("Type:");    lcd.setCursor(c2, y); lcd.print(t->filamentType); y += 26;
  lcd.setCursor(c1, y); lcd.print("Sub Type:");     lcd.setCursor(c2, y); lcd.print(t->detailedType); y += 26;
  lcd.setCursor(c1, y); lcd.print("Variant:"); lcd.setCursor(c2, y); lcd.print(t->variantId); y += 26;
  lcd.setCursor(c1, y); lcd.print("Material ID:");   lcd.setCursor(c2, y); lcd.print(t->materialId); y += 26;
  lcd.setCursor(c1, y); lcd.print("UID:");     lcd.setCursor(c2, y); lcd.printf("%02X%02X%02X%02X", t->uid[0], t->uid[1], t->uid[2], t->uid[3]); y += 26;
  //lcd.setCursor(c1, y); lcd.print("Color:");   lcd.setCursor(c2, y); lcd.printf("#%02X%02X%02X", t->colorR, t->colorG, t->colorB); 
  y += 26;
  lcd.setCursor(c1, y); lcd.print("Diameter:");    lcd.setCursor(c2, y); lcd.printf("%.2fmm", t->diameter); y += 26;
  lcd.setCursor(c1, y); lcd.print("Weight:");  lcd.setCursor(c2, y); lcd.printf("%dg", t->spoolWeight); y += 26;
  lcd.setCursor(c1, y); lcd.print("Length:");  lcd.setCursor(c2, y); lcd.printf("%dm", t->filamentLength); y += 26;
  lcd.setCursor(c1, y); lcd.print("Nozzle Temp:");  lcd.setCursor(c2, y); lcd.printf("%d-%dC", t->minNozzleTemp, t->maxNozzleTemp); y += 26;
  lcd.setCursor(c1, y); lcd.print("Bed Temp:");     lcd.setCursor(c2, y); lcd.printf("%dC", t->bedTemp); y += 26;
  lcd.setCursor(c1, y); lcd.print("Dry Temp:");     lcd.setCursor(c2, y); lcd.printf("%dC", t->dryTemp); y += 26;
  lcd.setCursor(c1, y); lcd.print("Dry Time:"); lcd.setCursor(c2, y); lcd.printf("%dh", t->dryTime);

  uint16_t swatch = lcd.color565(t->colorR, t->colorG, t->colorB);
  lcd.fillRoundRect(560, 135, 190, 90, 8, swatch);
  lcd.drawRoundRect(560, 135, 190, 90, 8, TFT_WHITE);
  lcd.setCursor(595, 245); lcd.printf("#%02X%02X%02X", t->colorR, t->colorG, t->colorB);

  drawFooter(); lcd.display();
}

// ──────────────────────────────────────────────────────────────
//  Draw dump-file selection list
// ──────────────────────────────────────────────────────────────
// ── FAT local file browser ─────────────────────────────────────
#define FAT_MAX_ENTRIES 64
struct FatEntry {
  char name[48];  // last path segment only
  bool isDir;
};
static FatEntry fatEntries[FAT_MAX_ENTRIES];
static int fatCount = 0;   // entries in current dir (excl. <BACK)
static int fatSel = 0;     // selected row (0 = <BACK when depth>0)
static int fatScroll = 0;  // top-visible row index
#define FAT_MAX_DEPTH 8
static String fatDirStack[FAT_MAX_DEPTH];  // ancestor paths
static int fatDepth = 0;
static String fatCurPath = "/";  // directory currently shown

// ──────────────────────────────────────────────────────────────
//  GitHub OLED browser state
// ──────────────────────────────────────────────────────────────
#define GH_MAX_ENTRIES 48
struct GhEntry {
  char name[48];   // display name
  char path[128];  // repo-relative path (e.g. "PLA/PLA Basic/Black")
  bool isDir;
};
static GhEntry ghEntries[GH_MAX_ENTRIES];
static int ghCount = 0;   // entries in current level
static int ghSel = 0;     // selected index
static int ghScroll = 0;  // top-visible index
#define GH_MAX_DEPTH 8
static String ghStack[GH_MAX_DEPTH];  // path at each navigation depth
static int ghDepth = 0;
static String ghDlStatus;  // result message after download

// ──────────────────────────────────────────────────────────────
//  BambuMan catalog OLED browser state
// ──────────────────────────────────────────────────────────────
#define BM_MAX_ENTRIES 64
struct BmCatEntry {
  char label[32];
};
static BmCatEntry bmCatEntries[BM_MAX_ENTRIES];
static int bmCatCount = 0;
static int bmCatSel = 0;
static int bmCatScroll = 0;
static int bmCatLevel = 0;  // 0=material, 1=type, 2=color, 3=uid
static char bmCatMat[32] = "";
static char bmCatType[32] = "";
static char bmCatColor[32] = "";

// ── BambuMan 3-level navigation cache (Mat / Type / Color) ────
// Built once from catalog.json on first browse; invalidated after sync.
// Levels 0–2 are served from RAM; level 3 (UIDs) always streams the file.
#define BM_CACHE_L0  24    // max distinct materials
#define BM_CACHE_L1  96    // max distinct mat+type combos
#define BM_CACHE_L2  128   // max distinct mat+type+color combos

struct BmCacheL0E { char mat[32]; };
struct BmCacheL1E { char mat[32]; char type[32]; };
struct BmCacheL2E { char mat[32]; char type[32]; char color[32]; };

static BmCacheL0E bmCL0[BM_CACHE_L0];
static int        bmCL0n = 0;
static BmCacheL1E bmCL1[BM_CACHE_L1];
static int        bmCL1n = 0;
static BmCacheL2E bmCL2[BM_CACHE_L2];
static int        bmCL2n = 0;
static bool       bmCacheValid = false;

// Total visible rows
inline int fatTotalRows() {
  return fatCount + (fatDepth > 0 ? 1 : 0);
}

void drawFatBrowser() {
  lcd.fillScreen(TFT_BLACK);

  drawStatusBar(); drawSubHeader("Write Tag");

  // Breadcrumb
  if (fatDepth > 0) {
    lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK); lcd.setTextSize(2);
    lcd.setCursor(12, 120);
    String crumb = fatCurPath;
    if (crumb.length() > 40) crumb = "..." + crumb.substring(crumb.length() - 37);
    lcd.print(crumb);
  }

  int total = fatTotalRows();
  int scroll = fatScroll;
  for (int i = 0; i < LIST_MAX_VIS && (scroll + i) < total; i++) {
    int rowIdx = scroll + i;
    int y = LIST_ROW_Y0 + i * LIST_ROW_H;
    bool sel = (rowIdx == fatSel);

    String label;
    if (rowIdx == 0 && fatDepth > 0) {
      lcd.setTextSize(2);
      label = "< BACK";
    } else {
      lcd.setTextSize(3);
      int ei = (fatDepth > 0) ? rowIdx - 1 : rowIdx;
      if (fatEntries[ei].isDir) label = String(fatEntries[ei].name);
      else {
        label = String(fatEntries[ei].name);
        if (label.endsWith(".bin")) label = label.substring(0, label.length() - 4);
      }
      if (label.length() > 22) label = label.substring(0, 21) + "~";
    }

    int bw = LIST_BTN_W, bh = LIST_BTN_H;
    int bx = 8;
    drawBtn(bx, y, bw, bh, TFT_DARKGREY, label.c_str());
  }
  if (fatCount == 0 && fatDepth > 0) {
            int delY = LIST_ROW_Y0 + LIST_ROW_H;
    drawBtn(8, delY, LIST_BTN_W, LIST_BTN_H, TFT_MAROON, "DELETE EMPTY FOLDER");
  }
  if (total > 0) drawScrollbar(scroll, total, LIST_ROW_Y0, LIST_MAX_VIS * LIST_ROW_H);
  drawFooter(); lcd.display();
}

// ──────────────────────────────────────────────────────────────
//  WiFi helpers
// ──────────────────────────────────────────────────────────────
String wifiSSID, wifiPass, ghToken;
bool apMode = false;

// Save WiFi credentials to FFat as JSON (mirrors NVS storage)
static void wifiSaveJson(const String& ssid, const String& pass) {
  if (!FFat.exists("/") || !FFat.begin(false)) {
    DBGLN("[WiFi]  FFat not available — skipping JSON save");
    return;
  }
  DynamicJsonDocument doc(128);
  doc["ssid"] = ssid;
  doc["pass"] = pass;
  File f = FFat.open("/wifi.json", FILE_WRITE);
  if (f) {
    serializeJson(doc, f);
    f.close();
    DBGLN("[WiFi]  Saved /wifi.json");
  }
}

// Load WiFi credentials from FFat JSON (fallback when NVS is empty)
static bool wifiLoadJson(String& ssid, String& pass) {
  if (!FFat.exists("/wifi.json")) return false;
  File f = FFat.open("/wifi.json", FILE_READ);
  if (!f) return false;
  DynamicJsonDocument doc(128);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  ssid = doc["ssid"] | "";
  pass = doc["pass"] | "";
  return !ssid.isEmpty();
}

void wifiLoadCreds() {
  DBGLN("[WiFi]  Loading credentials from NVS...");
  prefs.begin("wifi", true);
  wifiSSID = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  ghToken = prefs.getString("ghtoken", "");
  prefs.end();

  // Fall back to FFat JSON if NVS is empty
  if (wifiSSID.isEmpty()) {
    String jsonSSID, jsonPass;
    if (wifiLoadJson(jsonSSID, jsonPass)) {
      DBGF("[WiFi]  Loaded from /wifi.json: %s\n", jsonSSID.c_str());
      wifiSSID = jsonSSID;
      wifiPass = jsonPass;
    }
  }
}

void wifiSaveCreds(const String& ssid, const String& pass) {
  DBGF("[WiFi]  Saving credentials for SSID: %s\n", ssid.c_str());
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  wifiSSID = ssid;
  wifiPass = pass;
  wifiSaveJson(ssid, pass);  // also persist to FFat
}

// ── GitHub token helpers ───────────────────────────────────────────────
void ghTokenSave(const String& token) {
  DBGF("[WiFi]  Saving GitHub token (%d chars)\n", token.length());
  prefs.begin("wifi", false);
  prefs.putString("ghtoken", token);
  prefs.end();
  ghToken = token;
}

// Adds User-Agent, Accept, and (if configured) Bearer Authorization to every GitHub request.
void ghAddHeaders(HTTPClient& http) {
  http.addHeader("User-Agent", "BambuTagger/1.0");
  http.addHeader("Accept", "application/vnd.github.v3+json");
  if (ghToken.length() > 0)
    http.addHeader("Authorization", "Bearer " + ghToken);
}

bool wifiConnect() {
  DBGF("[WiFi]  Connecting to SSID: %s ...\n", wifiSSID.c_str());
  if (wifiSSID.isEmpty()) return false;
  showStatus2("Connecting WiFi", wifiSSID.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  for (int i = 0; i < 24 && WiFi.status() != WL_CONNECTED; i++)
    delay(500);
  return WiFi.status() == WL_CONNECTED;
}

void wifiStartAP() {
  DBGLN("[WiFi]  Starting AP mode...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  apMode = true;
  showStatus2("AP: " AP_SSID, "http://192.168.4.1");
  delay(1500);
}

// ──────────────────────────────────────────────────────────────
//  Embedded web interface HTML  (stored in flash via PROGMEM)
// ──────────────────────────────────────────────────────────────
static const char INDEX_HTML[] PROGMEM = R"HTMLRAW(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BambuTagger</title>
<link rel="icon" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgAgMAAAAOFJJnAAABhWlDQ1BJQ0MgcHJvZmlsZQAAKJF9kb9Lw0AcxV9bS6VUHawg4pChOrWLijiWKhbBQmkrtOpgcukvaNKQpLg4Cq4FB38sVh1cnHV1cBUEwR8g/gHipOgiJX4vKbSI8eC4D+/uPe7eAd5WjSlGXxxQVFPPJBNCvrAqBF7hRxAjGERUZIaWyi7m4Dq+7uHh612MZ7mf+3MMyEWDAR6BOM403STeIJ7dNDXO+8RhVhFl4nPiqE4XJH7kuuTwG+eyzV6eGdZzmXniMLFQ7mGph1lFV4hniCOyolK+N++wzHmLs1JrsM49+QtDRXUly3Wa40hiCSmkIUBCA1XUYCJGq0qKgQztJ1z8Y7Y/TS6JXFUwciygDgWi7Qf/g9/dGqXpKScplAD8L5b1MQEEdoF207K+jy2rfQL4noErteuvt4C5T9KbXS1yBAxtAxfXXU3aAy53gNEnTdRFW/LR9JZKwPsZfVMBGL4FgmtOb519nD4AOepq+QY4OAQmy5S97vLu/t7e/j3T6e8HrYRyvp7c8c0AAAAJUExURXIA83m/boC9efRkY8YAAAABdFJOUwBA5thmAAAAAWJLR0QAiAUdSAAAAL1JREFUGNNNkLEKg0AMhv8GHO52H0FR36SbCJHD6XASn+Lazb1XHG8R1Kds7kqLgZAvGZL/D3CJbXCp1sxTAs/cx5HmvWErUJhvYgsA9QJv6AAvzUqeXe5Au5qPNrMyL4GXslCuAppbK4B6AhkUDr6PUDpYBQiUgZw2Bs02KJsn4KVjgWLh+8idQbawVTwaqGeERwttfZv1coKMXrMgR2lFVcX9f2GIm5PUwpBL4omPOdlJBpNT9bOM87y+5AM/WTesHvLO9wAAAABJRU5ErkJggg==" type="image/svg+xml" />
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:#0d1117;color:#c9d1d9;min-height:100vh}
a{text-decoration:none;color:#c9d1d9;}
a:hover{text-decoration:none;color:#efefef;}
.nav{background:#161b22;border-bottom:1px solid #30363d;padding:12px 20px;display:flex;align-items:center;gap:20px}
.nav h1{color:#c9d1d9;font-size:1.2em;flex:1}
.nav .pill{background:#21262d;border-radius:20px;padding:4px 12px;font-size:.8em;cursor:pointer;border:1px solid #30363d;color:#c9d1d9}
.nav .pill.active{background:#1f6feb;border-color:#1f6feb;color:#fff}
.footer{left: 0;position: fixed;text-align: center;bottom: 0;width: 100%;background:#161b22;padding:12px 20px;align-items:center;gap:20px}
.content{max-width:700px;margin:20px auto;padding:0 16px}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:16px;margin-bottom:16px}
.card h3{color:#58a6ff;margin-bottom:12px;font-size:1em}
.card h4{color:#58a6ff;margin-bottom:12px;font-size:0.8em}
label{display:block;font-size:.8em;color:#8b949e;margin:8px 0 3px}
input[type=text],input[type=password],select{width:100%;padding:8px 10px;border-radius:6px;border:1px solid #30363d;background:#0d1117;color:#c9d1d9;font-size:.9em}
input:focus,select:focus{outline:2px solid #1f6feb;border-color:#1f6feb}
.btn{display:inline-block;padding:8px 18px;border-radius:6px;border:none;cursor:pointer;font-size:.85em;font-weight:600;margin:4px 4px 0 0;transition:.15s}
.btn-primary{background:#1f6feb;color:#fff}.btn-primary:hover{background:#388bfd}
.btn-success{background:#238636;color:#fff}.btn-success:hover{background:#2ea043}
.btn-danger{background:#b62324;color:#fff}.btn-danger:hover{background:#da3633}
.btn-secondary{background:#21262d;color:#c9d1d9;border:1px solid #30363d}
.btn-secondary:hover{background:#30363d}
.status{padding:10px 14px;border-radius:6px;font-size:.85em;margin:10px 0}
.ok{background:#0f3d2c;color:#3fb950;border:1px solid #238636}
.err{background:#3d0a0a;color:#f85149;border:1px solid #b62324}
.info{background:#102030;color:#79c0ff;border:1px solid #1f6feb}
.tree{list-style:none}
.tree li{padding:8px 12px;border-bottom:1px solid #21262d;cursor:pointer;display:flex;align-items:center;gap:8px;transition:.1s}
.tree li:hover{background:#21262d}
.tree .dir::before{content:"📁"}
.tree .file::before{content:"💾"}
.tree .back::before{content:"⬅️"}
.breadcrumb{font-size:.8em;color:#8b949e;margin-bottom:10px}
.breadcrumb a{color:#58a6ff;cursor:pointer;text-decoration:none}
.breadcrumb a:hover{text-decoration:underline}
.file-entry{display:flex;justify-content:space-between;align-items:center;padding:8px 12px;border-bottom:1px solid #21262d}
.file-name{color:#c9d1d9;font-size:.85em;flex:1}
.file-size{color:#8b949e;font-size:.75em;margin-right:12px}
.tag-table td{padding:4px 10px 4px 0;font-size:.85em}
.tag-table td:first-child{color:#8b949e;white-space:nowrap}
.swatch{display:inline-block;width:14px;height:14px;border-radius:3px;border:1px solid #30363d;vertical-align:middle;margin-left:6px}
.spinner{display:inline-block;width:14px;height:14px;border:2px solid #30363d;border-top-color:#58a6ff;border-radius:50%;animation:spin .6s linear infinite;margin-right:6px;vertical-align:middle}
@keyframes spin{to{transform:rotate(360deg)}}
.hidden{display:none}
</style>
</head>
<body>
<div class="nav">
  <h1><img style="vertical-align:middle" src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgAgMAAAAOFJJnAAABhWlDQ1BJQ0MgcHJvZmlsZQAAKJF9kb9Lw0AcxV9bS6VUHawg4pChOrWLijiWKhbBQmkrtOpgcukvaNKQpLg4Cq4FB38sVh1cnHV1cBUEwR8g/gHipOgiJX4vKbSI8eC4D+/uPe7eAd5WjSlGXxxQVFPPJBNCvrAqBF7hRxAjGERUZIaWyi7m4Dq+7uHh612MZ7mf+3MMyEWDAR6BOM403STeIJ7dNDXO+8RhVhFl4nPiqE4XJH7kuuTwG+eyzV6eGdZzmXniMLFQ7mGph1lFV4hniCOyolK+N++wzHmLs1JrsM49+QtDRXUly3Wa40hiCSmkIUBCA1XUYCJGq0qKgQztJ1z8Y7Y/TS6JXFUwciygDgWi7Qf/g9/dGqXpKScplAD8L5b1MQEEdoF207K+jy2rfQL4noErteuvt4C5T9KbXS1yBAxtAxfXXU3aAy53gNEnTdRFW/LR9JZKwPsZfVMBGL4FgmtOb519nD4AOepq+QY4OAQmy5S97vLu/t7e/j3T6e8HrYRyvp7c8c0AAAAJUExURXIA83m/boC9efRkY8YAAAABdFJOUwBA5thmAAAAAWJLR0QAiAUdSAAAAL1JREFUGNNNkLEKg0AMhv8GHO52H0FR36SbCJHD6XASn+Lazb1XHG8R1Kds7kqLgZAvGZL/D3CJbXCp1sxTAs/cx5HmvWErUJhvYgsA9QJv6AAvzUqeXe5Au5qPNrMyL4GXslCuAppbK4B6AhkUDr6PUDpYBQiUgZw2Bs02KJsn4KVjgWLh+8idQbawVTwaqGeERwttfZv1coKMXrMgR2lFVcX9f2GIm5PUwpBL4omPOdlJBpNT9bOM87y+5AM/WTesHvLO9wAAAABJRU5ErkJggg=="> BambuTagger</h1>
  <div class="pill active"  id="tab-local-btn"  onclick="switchTab('local')">Local Library</div>
  <div class="pill"         id="tab-github-btn"   onclick="switchTab('github')">GitHub Library</div>
  <div class="pill"         id="tab-bambuman-btn" onclick="switchTab('bambuman')">BambuMan Library</div>
  <div class="pill"         id="tab-status-btn" onclick="switchTab('status')">System</div>
  <div class="pill"         id="tab-ota-btn"    onclick="switchTab('ota')">OTA Update</div>
  <div class="pill"         id="tab-wifi-btn"   onclick="switchTab('wifi')">Config</div>
</div>

<div class="content">
<!-- ── WIFI TAB ─────────────────────────────────────────── -->
<div id="tab-wifi" class="hidden">
  <div class="card">
    <h3>Configuration</h3>
    <p style="font-size:.85em;color:#8b949e;margin:0 0 12px">
      Configure WiFi and GitHub API token..
    </p>
  </div>
  <div class="card">
    <h4>WiFi Configuration</h4>
    <div id="wstatus" class="status info">Checking…</div>
    <label>Network (SSID)</label>
    <input type="text" id="wifi-ssid" placeholder="Your WiFi name">
    <label>Password</label>
    <input type="password" id="wifi-pass" placeholder="Password (leave blank if open)">
    <br><br>
    <button class="btn" onclick="saveWifi()">💾 Save &amp; Connect</button>
    <button class="btn" onclick="scanNets()">🔍 Scan</button>
    <div id="nets" style="margin-top:10px"></div>
  </div>
  <div class="card">
    <h4>GitHub API Token</h4>
    <label style="margin-top:12px">GitHub API Token <span style="color:#8b949e;font-size:.8em">(optional &mdash; avoids rate&nbsp;limits)</span></label>
    <input type="password" id="gh-token" placeholder="ghp_…" autocomplete="off">
    <br><br>
    <button class="btn" onclick="saveToken()">🔑 Save Token</button>
  </div>
</div>

<!-- ── GITHUB TAB ────────────────────────────────────────── -->
<div id="tab-github" class="hidden">
  <div class="card">
    <h3>GitHub Library</h3>
    <p style="font-size:.85em;color:#8b949e;margin:0 0 12px">
      900+ tags from <a href="" target="_blank"  style="color:#58a6ff">Bambu-Lab-RFID-Library</a>.<br>
    </p>
  </div>
  <div class="card">
    <div class="breadcrumb" id="crumb">
      <a onclick="githubNav('')">Root</a>
    </div>
    <div id="gh-tree"><div class="status info">Tap a folder to browse…</div></div>
    <div id="dl-msg" style="margin-top:8px"></div>
  </div>
</div>

<!-- ── LOCAL FILES TAB ───────────────────────────────────── -->
<div id="tab-local">
  <div class="card">
    <h3>Local Library</h3>
    <p style="font-size:.85em;color:#8b949e;margin:0 0 12px">
      Upload, browse and write tags from local library.
    </p>
  </div>
  <!-- Upload card -->
  <div class="card">
    <h4>Upload Tag File</h4>
    <div id="drop-zone"
         ondragover="event.preventDefault();this.classList.add('drag-over')"
         ondragleave="this.classList.remove('drag-over')"
         ondrop="handleDrop(event)"
         onclick="document.getElementById('upload-input').click()">
      📂 Drag &amp; drop a <code>.bin</code> file here, or click to browse
    </div>
    <input type="file" id="upload-input" accept=".bin" style="display:none" onchange="uploadFile(this.files[0])">
    <div id="upload-msg" style="margin-top:8px"></div>
  </div>

  <!-- File list card -->
  <div class="card">
    <h4>Stored Tag Files</h4>
    <div class="breadcrumb" id="local-crumb"><a onclick="loadLocal('/')" style="cursor:pointer">Root</a></div>
    <div id="local-list"><div class="status info">Loading…</div></div>
    <button class="btn" style="margin-top:8px" onclick="loadLocal()">↻ Refresh</button>
  </div>

</div>


<!-- ── BAMBUMAN TAB ─────────────────────────────────────── -->
<div id="tab-bambuman" class="hidden">
   <div class="card">
    <h3>BambuMan Library</h3>
    <p style="font-size:.85em;color:#8b949e;margin:0 0 12px">
      2,600+ community tags from
      <a href="https://bambuman.ee/tags" target="_blank" style="color:#58a6ff">bambuman.ee</a>.<br>
      Sync the catalog once, then search by material or color name.
    </p>
  </div>

  <!-- Sync -->
  <div class="card" style="margin-bottom:10px">
    <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
      <button onclick="bmSync()" class="btn" id="bm-sync-btn">&#x1F504; Sync Catalog</button>
      <span id="bm-catalog-info" style="font-size:.8em;color:#8b949e">Not synced yet &#x2014; tap to download index</span>
    </div>
    <div id="bm-sync-status" style="margin-top:6px"></div>
  </div>

  <!-- Search -->
  <div class="card" style="margin-bottom:10px">
    <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:8px">
      <select id="bm-mat-filter" onchange="bmSearch()"
              style="background:#0d1117;border:1px solid #30363d;color:#e6edf3;
                     padding:6px 10px;border-radius:6px;font-size:.9em">
        <option value="">All Materials</option>
      </select>
      <input id="bm-name-filter" placeholder="Search color / name&#x2026;" oninput="bmSearch()"
             style="flex:1;min-width:140px;background:#0d1117;border:1px solid #30363d;color:#e6edf3;
                    padding:6px 10px;border-radius:6px;font-size:.9em">
      <span id="bm-result-count" style="font-size:.8em;color:#8b949e;white-space:nowrap"></span>
    </div>
    <div id="bm-results" style="max-height:320px;overflow-y:auto;font-size:.85em"></div>
  </div>

  <!-- Fetch by UID -->
  <div class="card">
    <div class="section-title" style="font-size:.85em;margin:0 0 6px">Fetch by UID</div>
    <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
      <input id="bm-uid" placeholder="Tag UID (e.g. 9510C2A3)" maxlength="16"
             style="flex:1;min-width:140px;background:#0d1117;border:1px solid #30363d;color:#e6edf3;
                    padding:6px 10px;border-radius:6px;font-family:monospace;font-size:.9em">
      <button onclick="bmFetch()" class="btn" style="white-space:nowrap">&#x2B07; Fetch</button>
      <a href="https://bambuman.ee/tags" target="_blank" class="btn"
         style="text-decoration:none;white-space:nowrap">&#x1F517; Browse</a>
    </div>
    <div id="bm-status" style="margin-top:6px"></div>
  </div>
</div>

<!-- ── OTA TAB ───────────────────────────────────────────── -->
<div id="tab-ota" class="hidden">
  <div class="card">
    <h3>&#x1F504; OTA Firmware Update</h3>
    <p style="font-size:.85em;color:#8b949e;margin:0 0 12px">
      Fetch and flash the latest firmware release from
      <a href="https://github.com/VID-PRO/BambuTagger" target="_blank" style="color:#58a6ff">GitHub</a>
      over-the-air. The device will reboot after a successful update.
    </p>
  </div>
  <div class="card">
    <h4>Firmware Version</h4>
    <p>Current: <strong id="ota-cur">checking…</strong>&nbsp;&nbsp;
       Latest: <strong id="ota-latest">—</strong></p>
    <div id="ota-status" class="status info">Click <em>Check for Updates</em> to query GitHub.</div>
    <br>
    <button class="btn" onclick="otaCheck()">&#x1F50D; Check for Updates</button>
    <button class="btn" id="ota-flash-btn" style="display:none" onclick="otaFlashFw()">&#x2B06;&#xFE0F; Flash Update</button>
  </div>
</div>

<!-- ── STATUS TAB ────────────────────────────────────────── -->
<div id="tab-status" class="hidden">
  <div class="card">
    <h3>System</h3>
    <p style="font-size:.85em;color:#8b949e;margin:0 0 12px">
      Device system status and last read Tag.
    </p>
  </div>
  <div class="card">
    <h4>Device Status</h4>
    <div id="dev-status"><div class="status info">Loading…</div></div>
    <button class="btn btn-secondary" style="margin-top:8px" onclick="loadStatus()">↻ Refresh</button>
    <button class="btn btn-danger" style="margin-top:8px;margin-left:8px" onclick="deleteAllTags()">🗑 Delete All Tags</button>
  </div>
  <div class="card" id="last-tag-card" style="display:none">
    <h4>Last Read Tag</h4>
    <table class="tag-table" id="tag-table"></table>
  </div>
</div>
</div>

<!-- ── FOOTER ─────────────────────────────────────────────── -->
<div class="footer">
  <center>&copy; 2026 by <a href="https://www.bambutagger.de" target=_new>BambuTagger</a> | 
  credits to <a href="https://github.com/Bambu-Research-Group/RFID-Tag-Guide" target=_new>RFID-Tag-Guide</a> |
  Library from <a href="https://github.com/queengooborg/Bambu-Lab-RFID-Library" target=_new>Bambu-Lab-RFID-Library</a> and <a href="https://bambuman.ee" target=_new>BambuMan</a>
  </center>
</div>
<!-- /content -->

<script>
let curPath = '';
let pathStack = [];

function switchTab(name) {
  ['wifi','github','local','status','bambuman','ota'].forEach(t => {
    document.getElementById('tab-'+t).classList.toggle('hidden', t!==name);
    document.getElementById('tab-'+t+'-btn').classList.toggle('active', t===name);
  });
  if(name==='github' && curPath==='' && document.getElementById('gh-tree').textContent.includes('Click')) githubNav('');
  if(name==='local')    loadLocal();
  if(name==='bambuman') { loadBmList(); bmLoadCatalog(); }
  if(name==='status')   loadStatus();
  if(name==='wifi')     loadWifiStatus();
  if(name==='ota')      otaLoadVersion();
}

// ── OTA Update ──────────────────────────────────────────────
function otaLoadVersion() {
  fetch('/api/ota/check').then(r=>r.json()).then(d=>{
    document.getElementById('ota-cur').textContent = d.current || '?';
  }).catch(()=>{});
}
function otaCheck() {
  const st  = document.getElementById('ota-status');
  const btn = document.getElementById('ota-flash-btn');
  st.className = 'status info'; st.textContent = 'Querying GitHub…';
  btn.style.display = 'none';
  fetch('/api/ota/check').then(r=>r.json()).then(d=>{
    document.getElementById('ota-cur').textContent = d.current || '?';
    if(!d.ok){ st.className='status error'; st.textContent='Error: '+(d.error||'unknown'); return; }
    document.getElementById('ota-latest').textContent = d.latest || '?';
    // semver compare: only show flash button when latest strictly > current
    function semverGt(a, b) {
      const pa = String(a).replace(/^v/i,'').split('.').map(Number);
      const pb = String(b).replace(/^v/i,'').split('.').map(Number);
      for(let i=0;i<3;i++){ const x=pa[i]||0, y=pb[i]||0; if(y>x) return true; if(y<x) return false; }
      return false;
    }
    const newer = d.update_available && semverGt(d.current||'0', d.latest||'0');
    if(newer){
      st.className = 'status success';
      st.innerHTML = '&#x2B06;&#xFE0F; Update available: <strong>'+d.latest+'</strong>';
      btn.setAttribute('data-url', d.download_url||'');
      btn.style.display = '';
    } else {
      st.className = 'status success';
      st.textContent = '\u2705 Already up to date! ('+d.current+')';
    }
  }).catch(e=>{ st.className='status error'; st.textContent='Check failed: '+e; });
}
function otaFlashFw() {
  if(!confirm('Flash firmware update? The device will reboot automatically.')) return;
  const st  = document.getElementById('ota-status');
  const btn = document.getElementById('ota-flash-btn');
  st.className = 'status info'; st.textContent = '\u23F3 Downloading and flashing\u2026 do not close this page.';
  btn.disabled = true;
  fetch('/api/ota/update',{method:'POST'}).then(r=>r.json()).then(d=>{
    if(d.ok){ st.className='status success'; st.textContent='\u2705 Update complete! Device is rebooting\u2026'; }
    else    { st.className='status error';   st.textContent='\u274C Failed: '+(d.error||'unknown'); btn.disabled=false; }
  }).catch(()=>{
    // Device rebooted — connection dropped; treat as success
    st.className='status success'; st.textContent='\u2705 Device rebooting\u2026 reconnect in a few seconds.';
  });
}


// ── BambuMan Library ────────────────────────────────────────
let bmCatalog = null;

function bmSync() {
  const btn = document.getElementById('bm-sync-btn');
  const st  = document.getElementById('bm-sync-status');
  btn.disabled = true; btn.textContent = '\u23F3 Syncing\u2026';
  st.innerHTML = '<div class=\"status info\">\u23F3 Downloading catalog from bambuman.ee (may take 30\u201360 s)\u2026</div>';
  fetch('/api/bm/sync', {method:'POST'})
    .then(r => r.json())
    .then(d => {
      if (d.ok) {
        st.innerHTML = '<div class=\"status ok\">\u2713 Synced ' + d.count + ' tags</div>';
        bmCatalog = null;
        bmLoadCatalog();
      } else {
        st.innerHTML = '<div class=\"status err\">\u2717 ' + (d.error||'Sync failed') + '</div>';
      }
    })
    .catch(e => { st.innerHTML = '<div class=\"status err\">Request failed: ' + e + '</div>'; })
    .finally(() => { btn.disabled=false; btn.innerHTML='\u1F504 Sync Catalog'; });
}

function bmLoadCatalog() {
  fetch('/api/bm/catalog')
    .then(r => { if (!r.ok) throw new Error('not synced'); return r.json(); })
    .then(data => {
      bmCatalog = data;
      const mats = [...new Set(data.map(e=>e.m))].sort();
      const sel = document.getElementById('bm-mat-filter');
      sel.innerHTML = '<option value="">All Materials (' + data.length + ')</option>' +
        mats.map(m => '<option value=\"'+m+'\">'+m+'</option>').join('');
      document.getElementById('bm-catalog-info').textContent =
        data.length + ' entries \u2014 ready to search';
      bmSearch();
    })
    .catch(() => {
      document.getElementById('bm-catalog-info').textContent =
        'Not synced yet \u2014 click Sync Catalog';
    });
}

function bmSearch() {
  if (!bmCatalog) return;
  const mat  = document.getElementById('bm-mat-filter').value;
  const name = document.getElementById('bm-name-filter').value.toLowerCase().trim();
  let filtered = bmCatalog;
  if (mat)  filtered = filtered.filter(e => e.m === mat);
  if (name) filtered = filtered.filter(e =>
    e.t.toLowerCase().includes(name) ||
    e.c.toLowerCase().includes(name) ||
    e.u.toLowerCase().includes(name));
  document.getElementById('bm-result-count').textContent = filtered.length + ' results';
  const el = document.getElementById('bm-results');
  if (!filtered.length) {
    el.innerHTML = '<div class=\"status info\">No matches.</div>'; return;
  }
  const show = filtered.slice(0, 100);
  el.innerHTML =
    '<table style=\"width:100%;border-collapse:collapse\">' +
    '<thead><tr style=\"color:#8b949e;border-bottom:1px solid #30363d\">' +
    '<th style=\"text-align:left;padding:3px 5px\">UID</th>' +
    '<th style=\"text-align:left;padding:3px 5px\">Material</th>' +
    '<th style=\"text-align:left;padding:3px 5px\">Type</th>' +
    '<th style=\"text-align:left;padding:3px 5px\">Color</th>' +
    '<th style=\"padding:3px 5px\"></th></tr></thead><tbody>' +
    show.map(e => {
            const fp = buildBmPath(e.m, e.t, e.c, e.u);
      return '<tr style=\"border-bottom:1px solid #21262d\">' +
        '<td style=\"padding:3px 5px;font-family:monospace\">' + e.u + '</td>' +
        '<td style=\"padding:3px 5px\">' + e.m + '</td>' +
        '<td style=\"padding:3px 5px\">' + e.t + '</td>' +
        '<td style=\"padding:3px 5px\">' + e.c + '</td>' +
        '<td style=\"padding:3px 5px;white-space:nowrap\">' +
        '<button class=\"btn\" style=\"padding:2px 7px;font-size:.75em;margin-right:3px\"' +
        'onclick=\"bmFetchEntry(\'' + e.u + '\',\'' + e.m.replace('/g','\\') + '\',\'' + e.t.replace('/g','\\') + '\',\'' + e.c.replace('/g','\\') + '\')\">' +
        '\u2B07 Download</button>' +
        '</td></tr>';
    }).join('') + '</tbody></table>' +
    (filtered.length > 100
      ? '<div style=\"font-size:.8em;color:#8b949e;padding:4px\">Showing 100 of ' +
        filtered.length + ' \u2014 refine search to see more.</div>'
      : '');
}

function normBmSeg(s) { return s.toUpperCase().replace(/ /g, '_'); }
function buildBmPath(m, t, c, u) {
  return '/' + normBmSeg(m) + '/' + normBmSeg(t) + '/' + normBmSeg(c) + '/' + u + '.bin';
}

function bmFetchUid(uid) {
  document.getElementById('bm-uid').value = uid;
  bmFetch();
}

// Called from search results where we have full m/t/c info
function bmFetchEntry(uid, mat, typ, col) {
  const st = document.getElementById('bm-status');
  if (st) st.innerHTML = '<div class=\"status info\">⏳ Fetching ' + uid + '…</div>';
  const params = new URLSearchParams({uid, mat, type: typ, color: col});
  fetch('/api/bm/fetch?' + params)
    .then(r => r.json())
    .then(d => {
      if (d.ok) {
        if (st) st.innerHTML = '<div class=\"status ok\">✓ Saved as ' + d.path + ' (' + d.size + ' B)</div>';
        loadBmList();
      } else {
        if (st) st.innerHTML = '<div class=\"status err\">✗ ' + d.error + '</div>';
      }
    })
    .catch(e => { if (st) st.innerHTML = '<div class=\"status err\">Request failed: ' + e + '</div>'; });
}

function bmFetch() {
  const uid = document.getElementById('bm-uid').value.trim().toUpperCase();
  const st  = document.getElementById('bm-status');
  if (!uid) { st.innerHTML = '<div class=\"status err\">Enter a UID first.</div>'; return; }
  st.innerHTML = '<div class=\"status info\">\u23F3 Fetching from bambuman.ee\u2026</div>';
  fetch('/api/bm/fetch?uid=' + encodeURIComponent(uid))
    .then(r => r.json())
    .then(d => {
      if (d.ok) {
        st.innerHTML = '<div class=\"status ok\">\u2713 Saved as ' + d.path + ' (' + d.size + ' B)</div>';
        loadBmList();
      } else {
        st.innerHTML = '<div class=\"status err\">\u2717 ' + d.error + '</div>';
      }
    })
    .catch(e => { st.innerHTML = '<div class=\"status err\">Request failed: ' + e + '</div>'; });
}

function loadBmList() {
  fetch('/api/bm/list')
    .then(r => r.json())
    .then(files => {
      const el  = document.getElementById('bm-list');
      const cnt = document.getElementById('bm-count');
      cnt.textContent = '(' + files.length + ')';
      if (!files.length) {
        el.innerHTML = '<div class=\\"status info\\">No files yet.</div>';
        return;
      }
      el.innerHTML = files.map(e => {
        const sz   = e.size < 1024 ? e.size + ' B' : (e.size/1024).toFixed(1) + ' KB';
        const name = e.path.split('/').pop();
        return '<div class=\"file-entry\">' +
               '<span class=\"file-name\">&#128222; ' + name + '</span>' +
               '<span class=\"file-size\">' + sz + '</span>' +
               '<button class=\"btn\" style=\"padding:4px 8px;font-size:.75em;background:#2196F3;color:#fff;margin-right:4px\"' +
               'onclick=\"writeTagFromFile(\'' + e.path + '\')\">' +
               '\u270F Write</button>' +
               '<button class=\"btn btn-danger\" style=\"padding:4px 8px;font-size:.75em\"' +
               'onclick=\"bmDelFile(\'' + e.path + '\')\">' +
               '&#128465;</button></div>';
      }).join('');
    })
    .catch(() => {});
}

// ── WiFi ────────────────────────────────────────────────────
function loadWifiStatus() {
  fetch('/api/status').then(r=>r.json()).then(d=>{
    const el = document.getElementById('wstatus');
    el.className = 'status ' + (d.wifi ? 'ok' : 'err');
    el.innerHTML = d.wifi
      ? `✅ Connected to <b>${d.ssid}</b><br>Device IP: <b>${d.ip}</b>`
      : '❌ Not connected. Enter credentials below.';
    document.getElementById('wifi-ssid').value = d.ssid || '';
    fetch('/api/token').then(r=>r.json()).then(t=>{
      document.getElementById('gh-token').value = t.token || '';
    }).catch(()=>{});
  }).catch(()=>{});
}

function saveWifi() {
  const body = JSON.stringify({ssid:document.getElementById('wifi-ssid').value,
                               pass:document.getElementById('wifi-pass').value});
  fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body})
  .then(r=>r.json()).then(d=>{
    const el = document.getElementById('wstatus');
    el.className = 'status '+(d.success?'ok':'err');
    el.innerHTML = d.message;
    if(d.success) setTimeout(loadWifiStatus, 5000);
  });
}

function saveToken() {
  const tok = document.getElementById('gh-token').value.trim();
  fetch('/api/token',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({token:tok})})
  .then(r=>r.json()).then(d=>{
    const el = document.getElementById('wstatus');
    el.className = 'status '+(d.success?'ok':'err');
    el.innerHTML = d.message;
  }).catch(()=>{});
}

function scanNets() {
  document.getElementById('nets').innerHTML = '<div class="status info"><span class="spinner"></span>Scanning…</div>';
  fetch('/api/scan').then(r=>r.json()).then(arr=>{
    if(!arr.length){document.getElementById('nets').innerHTML='<div class="status info">No networks found</div>';return;}
    document.getElementById('nets').innerHTML = arr.map(n=>
      `<div style="padding:6px 10px;border-bottom:1px solid #21262d;cursor:pointer" onclick="document.getElementById('wifi-ssid').value='${n.ssid.replace(/'/g,"\\'")}'">`+
      `📶 <b>${n.ssid}</b> <span style="color:#8b949e">(${n.rssi} dBm)</span></div>`
    ).join('');
  });
}

// ── GitHub browser ─────────────────────────────────────────
function buildCrumb(path) {
  let html = '<a onclick="githubNav(\'\')">Root</a>';
  if(path){
    let parts = path.split('/'), acc = '';
    parts.forEach(p=>{
      acc = acc ? acc+'/'+p : p;
      const cp = acc;
      html += ' / <a onclick="githubNav(\''+cp+'\')">'+p+'</a>';
    });
  }
  document.getElementById('crumb').innerHTML = html;
}

function githubNav(path) {
  curPath = path;
  buildCrumb(path);
  document.getElementById('gh-tree').innerHTML = '<div class="status info"><span class="spinner"></span>Loading…</div>';
  fetch('/api/list?path='+encodeURIComponent(path))
  .then(r=>r.json()).then(items=>{
    let html = '<ul class="tree">';
    if(path) html += `<li class="back" onclick="githubNav('${path.includes('/')?path.substring(0,path.lastIndexOf('/')):''}')"> Back</li>`;
    items.forEach(it=>{
      if(it.type==='dir'){
        html += `<li class="dir" onclick="githubNav('${it.path}')"> ${it.name}</li>`;
      } else if(it.name.endsWith('.bin')){
        html += `<li class="file"> ${it.name}
          <button class="btn" style="margin-left:auto;margin-top:0;padding:4px 10px"
            onclick="dlDump('${it.path}','${it.name.replace(/'/g,"\\'")}')">\u2B07 Download</button></li>`;
      }
    });
    html += '</ul>';
    if(items.length===0) html = '<div class="status info">Empty folder</div>';
    document.getElementById('gh-tree').innerHTML = html;
  }).catch(e=>{
    document.getElementById('gh-tree').innerHTML = '<div class="status err">Error: '+e+'</div>';
  });
}

function dlDump(path, name) {
  document.getElementById('dl-msg').innerHTML = '<div class="status info"><span class="spinner"></span>Downloading '+name+'…</div>';
  fetch('/api/download',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({path,name})})
  .then(r=>r.json()).then(d=>{
    document.getElementById('dl-msg').innerHTML =
      `<div class="status ${d.success?'ok':'err'}">${d.message}</div>`;
  });
}

// ── Local files ────────────────────────────────────────────
var localPath = '/';

function loadLocal(dir) {
  if(dir !== undefined) localPath = dir;
  fetch('/api/files?dir='+encodeURIComponent(localPath))
  .then(r=>r.json()).then(data=>{
    let crumb = '<a onclick="loadLocal(\'\/\')" style="cursor:pointer">Root</a>';
    if(localPath !== '/') {
      const parts = localPath.split('/').filter(Boolean);
      let cum = '';
      parts.forEach((p,i)=>{
        cum += '/'+p;
        const cp = cum;
        if(i < parts.length-1)
          crumb += ' / <a onclick="loadLocal(\''+cp+'\')" style="cursor:pointer">'+p+'</a>';
        else
          crumb += ' / <strong>'+p+'</strong>';
      });
    }
    document.getElementById('local-crumb').innerHTML = crumb;
    const entries = data.entries || [];
    if(!entries.length && localPath==='/'){
      document.getElementById('local-list').innerHTML='<div class="status info">No tags yet. Use the Library tab.</div>';
      return;
    }
    let html = '';
    if(localPath !== '/'){
      const par = localPath.lastIndexOf('/')>0 ? localPath.substring(0,localPath.lastIndexOf('/')) : '/';
      html += '<div class="file-entry" onclick="loadLocal(\''+par+'\')" style="cursor:pointer"><span class="file-name">⬆ ..</span></div>';
    }
    entries.filter(e=>e.isDir).sort((a,b)=>a.name.localeCompare(b.name)).forEach(e=>{
      const cp = localPath==='/' ? '/'+e.name : localPath+'/'+e.name;
      html += '<div class="file-entry" onclick="loadLocal(\''+cp+'\')" style="cursor:pointer"><span class="file-name">📁 '+e.name+'</span></div>';
    });
    entries.filter(e=>!e.isDir).sort((a,b)=>a.name.localeCompare(b.name)).forEach(e=>{
      const fp = localPath==='/' ? '/'+e.name : localPath+'/'+e.name;
      const sz = e.size<1024 ? e.size+' B' : (e.size/1024).toFixed(1)+' KB';
      html += '<div class="file-entry"><span class="file-name">💾 '+e.name+'</span><span class="file-size">'+sz+'</span><button class="btn" style="padding:4px 8px;font-size:.75em;background:#2196F3;color:#fff;margin-right:4px" onclick="writeTagFromFile(\''+fp+'\')">\u270F Write</button><button class="btn btn-danger" style="padding:4px 8px;font-size:.75em" onclick="delFile(\''+fp+'\')">🗑</button></div>';
    });
    if(localPath!=='/' && entries.filter(e=>!e.isDir).length===0 && entries.filter(e=>e.isDir).length===0){
      html += '<div class="file-entry"><span class="file-name">📂 (empty)</span><button class="btn btn-danger" style="padding:4px 8px;font-size:.75em" onclick="delFolder(\''+localPath+'\')">🗑 Delete Folder</button></div>';
    }
    if(!html) html = '<div class="status info">Empty folder.</div>';
    document.getElementById('local-list').innerHTML = html;
  }).catch(e=>{
    document.getElementById('local-list').innerHTML='<div class="status err">Error: '+e+'</div>';
  });
}

function delFile(name) {
  if(!confirm('Delete '+name+'?')) return;
  fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({name})})
  .then(()=>loadLocal());
}

function delFolder(name) {
  if(!confirm('Delete folder '+name+'?')) return;
  const par = name.lastIndexOf('/')>0 ? name.substring(0,name.lastIndexOf('/')) : '/';
  fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({name})})
  .then(()=>loadLocal(par));
}

function deleteAllTags() {
  if(!confirm('Delete ALL dump tags and empty folders?\\nThis cannot be undone.')) return;
  fetch('/api/deleteall',{method:'POST'})
  .then(r=>r.json()).then(d=>{
    alert('Deleted '+d.count+' tags');
    loadStatus();
  });
}

function writeTagFromFile(path) {
  showWriteModal('Connecting\u2026');
  fetch('/api/writetag', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({path})})
  .then(r=>r.json()).then(d=>{
    if(!d.ok){ showWriteModal(null); alert('Error: '+d.message); return; }
    showWriteModal('\ud83d\udce1 Place tag on RFID reader\u2026\n\u23f3 20 second window');
    pollWriteState(0);
  }).catch(e=>{ showWriteModal(null); alert('Request failed: '+e); });
}

function pollWriteState(n) {
  if(n > 22) { showWriteModal(null); alert('Timed out waiting for tag.'); return; }
  setTimeout(()=>{
    fetch('/api/status').then(r=>r.json()).then(d=>{
      if(d.app_state === 'DUMP_WRITE') { pollWriteState(n+1); return; }
      showWriteModal(null);
      alert(d.app_state === 'MAIN_MENU' ? 'Write complete \u2713' : 'Done (state: '+d.app_state+')');
    }).catch(()=>pollWriteState(n+1));
  }, 1000);
}

function showWriteModal(msg) {
  let m = document.getElementById('write-modal');
  if(!msg) { if(m) m.remove(); return; }
  if(!m) {
    m = document.createElement('div');
    m.id = 'write-modal';
    m.style.cssText='position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.65);z-index:9999;display:flex;align-items:center;justify-content:center';
    m.innerHTML='<div style="background:#1e1e2e;border:1px solid #444;border-radius:10px;padding:32px 40px;text-align:center;max-width:320px"><div id="write-modal-msg" style="white-space:pre-line;font-size:1.1em;color:#e0e0e0;margin-bottom:16px"></div><button class="btn btn-secondary" onclick="showWriteModal(null)">Cancel</button></div>';
    document.body.appendChild(m);
  }
  document.getElementById('write-modal-msg').textContent = msg;
}

function handleDrop(e) {
  e.preventDefault();
  document.getElementById('drop-zone').classList.remove('drag-over');
  const f = e.dataTransfer.files[0];
  if(f) uploadFile(f);
}

function uploadFile(file) {
  if(!file) return;
  if(!file.name.toLowerCase().endsWith('.bin')) {
    document.getElementById('upload-msg').innerHTML=
      '<div class="status err">Only .bin files are accepted.</div>';
    return;
  }
  const msg = document.getElementById('upload-msg');
  msg.innerHTML='<div class="status info"><span class="spinner"></span>Uploading '+file.name+'…</div>';
  const fd = new FormData();
  fd.append('file', file, file.name);
  fetch('/api/upload',{method:'POST',body:fd})
  .then(r=>r.json()).then(d=>{
    msg.innerHTML='<div class="status '+(d.success?'ok':'err')+'">'+d.message+'</div>';
    if(d.success) loadLocal();
  }).catch(err=>{
    msg.innerHTML='<div class="status err">Upload error: '+err+'</div>';
  });
}

// ── Status ─────────────────────────────────────────────────
function loadStatus() {
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('dev-status').innerHTML = `
      <table class="tag-table">
        <tr><td>WiFi</td><td>${d.wifi?'✅ '+d.ssid:'❌ Not connected'}</td></tr>
        <tr><td>IP</td><td>${d.ip}</td></tr>
        <tr><td>Mode</td><td>${d.ap_mode?'Access Point (AP)':'Station (STA)'}</td></tr>
        <tr><td>Free Heap</td><td>${d.heap} bytes</td></tr>
        <tr><td>FAT</td><td>${d.fat_used / 1024} / ${d.fat_total / 1024} kbytes</td></tr>
        <tr><td>Tag Dumps</td><td>${d.dump_count||0} files</td></tr>
        <tr><td>Last Written Tag</td><td>${d.selected_dump||'— none —'}</td></tr>
      </table>`;

    if(d.last_tag && d.last_tag.valid) {
      const t = d.last_tag;
      const sw = `<span class="swatch" style="background:#${t.colorR.toString(16).padStart(2,'0')}${t.colorG.toString(16).padStart(2,'0')}${t.colorB.toString(16).padStart(2,'0')}"></span>`;
      document.getElementById('tag-table').innerHTML = `
        <tr><td>UID</td><td>${t.uid}</td></tr>
        <tr><td>Type</td><td>${t.filamentType}</td></tr>
        <tr><td>Sub-type</td><td>${t.detailedType}</td></tr>
        <tr><td>Variant</td><td>${t.variantId}</td></tr>
        <tr><td>Material ID</td><td>${t.materialId}</td></tr>
        <tr><td>Color</td><td>#${t.colorR.toString(16).padStart(2,'0').toUpperCase()}${t.colorG.toString(16).padStart(2,'0').toUpperCase()}${t.colorB.toString(16).padStart(2,'0').toUpperCase()} ${sw}</td></tr>
        <tr><td>Spool weight</td><td>${t.spoolWeight} g</td></tr>
        <tr><td>Diameter</td><td>${t.diameter.toFixed(2)} mm</td></tr>
        <tr><td>Length</td><td>${t.filamentLength} m</td></tr>
        <tr><td>Nozzle temp</td><td>${t.minNozzleTemp}–${t.maxNozzleTemp} °C</td></tr>
        <tr><td>Bed temp</td><td>${t.bedTemp} °C</td></tr>
        <tr><td>Dry temp/time</td><td>${t.dryTemp} °C / ${t.dryTime} h</td></tr>`;
      document.getElementById('last-tag-card').style.display='block';
    }
  });
}

loadWifiStatus();
</script>
</body>
</html>
)HTMLRAW";

// ──────────────────────────────────────────────────────────────
//  Web server – API handlers
// ──────────────────────────────────────────────────────────────

// Helper: serialise a TagInfo to a JSON object in doc
static void tagInfoToJson(JsonObject obj, const TagInfo* t) {
  char uid[9];
  snprintf(uid, sizeof(uid), "%02X%02X%02X%02X",
           t->uid[0], t->uid[1], t->uid[2], t->uid[3]);
  obj["valid"] = t->valid;
  obj["uid"] = uid;
  obj["filamentType"] = t->filamentType;
  obj["detailedType"] = t->detailedType;
  obj["variantId"] = t->variantId;
  obj["materialId"] = t->materialId;
  obj["colorR"] = t->colorR;
  obj["colorG"] = t->colorG;
  obj["colorB"] = t->colorB;
  obj["spoolWeight"] = t->spoolWeight;
  obj["diameter"] = t->diameter;
  obj["minNozzleTemp"] = t->minNozzleTemp;
  obj["maxNozzleTemp"] = t->maxNozzleTemp;
  obj["bedTemp"] = t->bedTemp;
  obj["dryTemp"] = t->dryTemp;
  obj["dryTime"] = t->dryTime;
  obj["filamentLength"] = t->filamentLength;
}

void apiStatus() {
  DBGLN("[HTTP]  GET /api/status");
  DynamicJsonDocument doc(1280);
  doc["wifi"] = (WiFi.status() == WL_CONNECTED);
  doc["ssid"] = wifiSSID;
  doc["ip"] = apMode ? "192.168.4.1" : WiFi.localIP().toString();
  doc["ap_mode"] = apMode;
  doc["heap"] = (int)ESP.getFreeHeap();
  doc["fat_total"] = (int)FFat.totalBytes();
  doc["fat_used"] = (int)FFat.usedBytes();
  doc["selected_dump"] = String(selectedDumpPath);
  int dumps = 0; countDumpFiles("/", dumps);
  doc["dump_count"] = dumps;
  static const char* stateNames[] = {
    "MAIN_MENU", "READ_TAG", "SHOW_TAG", "CLONE_SRC", "CLONE_TGT",
    "DUMP_SELECT", "DUMP_WRITE", "WIFI_INFO", "GH_BROWSE", "GH_DOWNLOAD",
    "BM_BROWSE", "BM_DOWNLOAD", "BM_CAT_BROWSE", "OTA_UPDATE"
  };
  doc["app_state"] = stateNames[(int)appState];

  JsonObject ltObj = doc.createNestedObject("last_tag");
  tagInfoToJson(ltObj, &currentTag);

  String out;
  serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

void apiWifi() {
  DBGF("[HTTP]  POST /api/wifi  method=%d\n", httpServer.method());
  DynamicJsonDocument doc(256);
  deserializeJson(doc, httpServer.arg("plain"));
  String ssid = doc["ssid"] | "";
  String pass = doc["pass"] | "";

  DynamicJsonDocument resp(128);
  if (ssid.isEmpty()) {
    resp["success"] = false;
    resp["message"] = "SSID cannot be empty";
  } else {
    wifiSaveCreds(ssid, pass);
    resp["success"] = true;
    resp["message"] = "Saved! Reconnecting in background";
  }
  String out;
  serializeJson(resp, out);
  httpServer.send(200, "application/json", out);

  if (!ssid.isEmpty()) {
    delay(300);
    WiFi.disconnect(true);
    delay(300);
    if (wifiConnect()) {
      apMode = false;
      Serial.println("Reconnected: " + WiFi.localIP().toString());
    }
  }
}

void apiScan() {
  DBGLN("[HTTP]  GET /api/scan");
  WiFi.scanDelete();
  int n = WiFi.scanNetworks(false, false, false, 200);
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n && i < 20; i++) {
    JsonObject o = arr.createNestedObject();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
  }
  String out;
  serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

// ── GitHub token API ──────────────────────────────────────────────────────
void apiTokenGet() {
  DBGLN("[HTTP]  GET /api/token");
  DynamicJsonDocument doc(128);
  doc["token"] = ghToken;
  String out;
  serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

void apiTokenSet() {
  DBGLN("[HTTP]  POST /api/token");
  DynamicJsonDocument doc(256);
  deserializeJson(doc, httpServer.arg("plain"));
  String token = doc["token"] | "";
  token.trim();
  ghTokenSave(token);
  DynamicJsonDocument resp(128);
  resp["success"] = true;
  if (token.length() > 0)
    resp["message"] = "Token saved (" + String(token.length()) + " chars)";
  else
    resp["message"] = "Token cleared";
  String out;
  serializeJson(resp, out);
  httpServer.send(200, "application/json", out);
}

/* Fetch GitHub API directory listing and return filtered JSON */
void apiList() {
  DBGLN("[HTTP]  GET /api/list");
  String path = httpServer.arg("path");

  path.replace(" ", "%20");

  if (WiFi.status() != WL_CONNECTED) {
    httpServer.send(200, "application/json", "[]");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();  // skip cert verification (ESP32 has no root CA store by default)
  HTTPClient http;
  String url = "https://" GITHUB_API_HOST "/repos" GITHUB_REPO_PATH "/contents/" + path;
  http.begin(client, url);
  ghAddHeaders(http);
  int code = http.GET();

  if (code != 200) {
    http.end();
    httpServer.send(200, "application/json", "[]");
    return;
  }

  // Filter: only keep name, path, type
  StaticJsonDocument<256> filter;
  filter[0]["name"] = true;
  filter[0]["path"] = true;
  filter[0]["type"] = true;

  DynamicJsonDocument raw(16384);
  DeserializationError err = deserializeJson(raw, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();

  DynamicJsonDocument resp(8192);
  JsonArray arr = resp.to<JsonArray>();

  if (!err) {
    for (JsonObject item : raw.as<JsonArray>()) {
      String type = item["type"] | "";
      String name = item["name"] | "";
      if (type == "file" && !name.endsWith(".bin")) continue;
      if (name.startsWith(".")) continue;
      JsonObject o = arr.createNestedObject();
      o["name"] = name;
      o["path"] = item["path"] | "";
      o["type"] = type;
    }
  }
  String out;
  serializeJson(resp, out);
  httpServer.send(200, "application/json", out);
}

// Build a descriptive FAT filename from a full GitHub repo path.
// e.g. "PLA/PLA Basic/Black/3AD82DAD/dump.bin" -> "PLA-PLA_BASIC-BLACK-3AD82DAD.bin"
// Build a FAT path that mirrors the GitHub repository directory structure.
// Example: "PLA/PLA Basic/Black/3AD82DAD/dump.bin"
//       →  "/PLA/PLA_BASIC/BLACK/3AD82DAD.bin"
// The leaf GitHub folder (UID) becomes the .bin filename; the ancestor
// directories are kept as FAT directory segments (uppercase, spaces→_).
String buildDumpFilePath(String repoPath) {
  // Remove URL encoding
  repoPath.replace("%20", " ");
  // Strip leading slash if present
  if (repoPath.startsWith("/")) repoPath = repoPath.substring(1);
  // Split into segments
  std::vector<String> segs;
  int start = 0;
  for (int i = 0; i <= (int)repoPath.length(); i++) {
    if (i == (int)repoPath.length() || repoPath[i] == '/') {
      segs.push_back(repoPath.substring(start, i));
      start = i + 1;
    }
  }
  // Drop the last segment (the actual file: "dump.bin" / "dump.json")
  if (segs.size() > 0) segs.pop_back();
  if (segs.empty()) return "/dump.bin";
  // Normalise each segment: uppercase, spaces→underscores
  for (auto& seg : segs) {
    seg.toUpperCase();
    seg.replace(" ", "_");
  }
  // Join with "/" – result is "/TYPE/SUBTYPE/COLOR/UID.bin"
  String result = "";
  for (const auto& seg : segs) {
    result += "/" + seg;
  }
  result += ".bin";
  return result;
}


// ── BambuMan structured path: /{MAT}/{TYPE}/{COLOR}/{UID}.bin ─────────────
String buildBmFilePath(const String& m, const String& t, const String& c, const String& uid) {
  return "/" + m + "/" + t + "/" + c + "/" + uid + ".bin";
}

// Stream-search /BM/catalog.json for a given UID; fill outMat/outType/outCol.
bool bmLookupCatalog(const String& uid, String& outMat, String& outType, String& outCol) {
  if (!FFat.exists("/BM/catalog.json")) return false;
  File f = FFat.open("/BM/catalog.json", "r");
  if (!f) return false;
  String needle = "\"u\":\"" + uid + "\"";
  String carry = "";
  carry.reserve(512);
  bool found = false;
  while (f.available() && !found) {
    char buf[128];
    int n = f.read((uint8_t*)buf, sizeof(buf) - 1);
    buf[n] = 0;
    carry += buf;
    int pos = carry.indexOf(needle);
    if (pos >= 0) {
      int start = carry.lastIndexOf('{', pos);
      int end = carry.indexOf('}', pos);
      if (start >= 0 && end > pos) {
        String obj = carry.substring(start, end + 1);
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, obj) == DeserializationError::Ok) {
          outMat = doc["m"] | "";
          outType = doc["t"] | "";
          outCol = doc["c"] | "";
          found = true;
        }
      }
    }
    // Keep overlap to avoid splitting across the needle
    if ((int)carry.length() > 512) carry = carry.substring(carry.length() - (int)needle.length() - 20);
  }
  f.close();
  return found;
}

// Append a BM file path to /BM/index.txt (deduplicated).
void bmIndexAdd(const String& path) {
  if (!FFat.exists("/BM")) FFat.mkdir("/BM");
  // Check for duplicate
  File fr = FFat.open("/BM/index.txt", "r");
  if (fr) {
    while (fr.available()) {
      String line = fr.readStringUntil('\n');
      line.trim();
      if (line == path) {
        fr.close();
        return;
      }
    }
    fr.close();
  }
  File fa = FFat.open("/BM/index.txt", "a");
  if (fa) {
    fa.println(path);
    fa.close();
  }
}

// GET /api/bm/list – return index of downloaded BM files; prune stale entries.
void apiBmList() {
  if (!FFat.exists("/BM/index.txt")) {
    httpServer.send(200, "application/json", "[]");
    return;
  }
  File f = FFat.open("/BM/index.txt", "r");
  if (!f) {
    httpServer.send(200, "application/json", "[]");
    return;
  }
  String out = "[", newIdx = "";
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (!FFat.exists(line)) continue;  // prune stale
    File fc = FFat.open(line, "r");
    int sz = fc ? (int)fc.size() : 0;
    if (fc) fc.close();
    if (!first) out += ",";
    first = false;
    String p = line;
    p.replace("\"", "\\\"");
    out += "{\"path\":\"" + p + "\",\"size\":" + sz + "}";
    newIdx += line + "\n";
  }
  f.close();
  out += "]";
  File fw = FFat.open("/BM/index.txt", "w");  // rewrite without stale entries
  if (fw) {
    fw.print(newIdx);
    fw.close();
  }
  httpServer.send(200, "application/json", out);
}

// Return the two innermost path segments for short OLED display.
// "/PLA/PLA_BASIC/BLACK/3AD82DAD.bin" → "BLACK/3AD82DAD"
String shortDumpName(const String& fullPath) {
  String p = fullPath;
  if (p.startsWith("/")) p = p.substring(1);
  if (p.endsWith(".bin")) p = p.substring(0, p.length() - 4);
  int last = p.lastIndexOf('/');
  if (last > 0) {
    int prev = p.lastIndexOf('/', last - 1);
    p = (prev >= 0) ? p.substring(prev + 1) : p.substring(last + 1);
  }
  return p;  // e.g. "BLACK/3AD82DAD"
}

// Ensure all parent directories in a FAT path exist.
// Required for LittleFS; harmless for legacy FFat.
void ensureParentDirs(const String& path) {
  for (int i = 1; i < (int)path.length(); i++) {
    if (path[i] == '/') {
      String dir = path.substring(0, i);
      if (!FFat.exists(dir)) FFat.mkdir(dir);
    }
  }
}

void apiDownload() {
  DynamicJsonDocument req(256);
  deserializeJson(req, httpServer.arg("plain"));

  String ghPath = req["path"] | "";
  ghPath.replace(" ", "%20");
  String fname = req["name"] | "";

  DBGF("[HTTP]  GET /api/download  url=%s\n", ghPath);

  DynamicJsonDocument resp(256);
  auto fail = [&](const char* msg) {
    resp["success"] = false;
    resp["message"] = msg;
    String out;
    serializeJson(resp, out);
    httpServer.send(200, "application/json", out);
  };

  if (ghPath.isEmpty() || fname.isEmpty()) {
    fail("Missing path/name");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    fail("WiFi not connected");
    return;
  }

  // Build descriptive filename from full repo path
  fname = buildDumpFilePath(ghPath);
  if (!fname.startsWith("/")) fname = "/" + fname;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, String(GITHUB_RAW_PREFIX) + ghPath);
  ghAddHeaders(http);
  int code = http.GET();
  if (code != 200) {
    fail(("HTTP " + String(code)).c_str());
    http.end();
    return;
  }

  int totalSize = http.getSize();
  if (totalSize > 0 && totalSize != DUMP_SIZE) {
    fail(("Bad size: " + String(totalSize)).c_str());
    http.end();
    return;
  }

  ensureParentDirs(fname);
  File f = FFat.open(fname, FILE_WRITE);
  if (!f) {
    fail("FFat open failed");
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[128];
  int written = 0;
  unsigned long t0 = millis();
  while (written < DUMP_SIZE && (millis() - t0) < 20000) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
      f.write(buf, n);
      written += n;
    } else if (!http.connected()) break;
    else delay(1);
  }
  f.close();
  http.end();

  if (written != DUMP_SIZE) {
    FFat.remove(fname);
    fail(("Incomplete: " + String(written) + "/" + String(DUMP_SIZE)).c_str());
    return;
  }

  resp["success"] = true;
  resp["message"] = "Saved as " + fname;
  String out;
  serializeJson(resp, out);
  httpServer.send(200, "application/json", out);
}

void apiFiles() {
  DBGLN("[HTTP]  GET /api/files");
  String dir = httpServer.hasArg("dir") ? httpServer.arg("dir") : "/";
  if (!dir.startsWith("/")) dir = "/" + dir;
  if (dir.length() > 1 && dir.endsWith("/"))
    dir = dir.substring(0, dir.length() - 1);
  DynamicJsonDocument doc(4096);
  JsonObject root = doc.to<JsonObject>();
  root["path"] = dir;
  JsonArray arr = root.createNestedArray("entries");
  File d = FFat.open(dir);
  if (d && d.isDirectory()) {
    File f = d.openNextFile();
    while (f) {
      String fn = f.name();
      int sl = fn.lastIndexOf('/');
      String bn = (sl >= 0) ? fn.substring(sl + 1) : fn;
      bool isDir = f.isDirectory();
      if ((isDir || bn.endsWith(".bin")) && !bn.endsWith("BM")) {
        JsonObject o = arr.createNestedObject();
        o["name"] = bn;
        o["isDir"] = isDir;
        if (!isDir) o["size"] = (int)f.size();
      }
      f = d.openNextFile();
    }
  }
  String out;
  serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

void apiDelete() {
  DBGF("[HTTP]  POST /api/delete  file=%s\n",
       httpServer.arg("file").c_str());
  DynamicJsonDocument doc(128);
  deserializeJson(doc, httpServer.arg("plain"));
  String name = doc["name"] | "";
  if (!name.startsWith("/")) name = "/" + name;
  bool ok = false;
  if (!name.isEmpty()) {
    File f = FFat.open(name);
    if (f && f.isDirectory()) {
      // Check if empty
      File child = f.openNextFile();
      ok = !child;  // true if no children
      if (ok) ok = FFat.rmdir(name);
    } else {
      ok = FFat.remove(name);
    }
  }
  httpServer.send(200, "application/json",
                  ok ? "{\"success\":true}" : "{\"success\":false}");
}

static void deleteAllDumpsRecursive(const String& path, int& count) {
  // First pass: count all .bin files for progress
  static int total = 0;
  total = 0;
  auto countBin = [](const String& p, auto& self) -> void {
    File dir = FFat.open(p);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f) {
      String sub = p == "/" ? "/" + String(f.name()) : p + "/" + f.name();
      if (f.isDirectory()) self(sub, self);
      else if (String(f.name()).endsWith(".bin")) total++;
      f = dir.openNextFile();
    }
  };
  countBin(path, countBin);

  // Second pass: delete and show progress
  int done = 0;
  unsigned long lastDraw = 0;
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("System");
  drawProgressBar(0, "Deleting tags...", "");

  auto delRec = [&](const String& p, auto& self) -> void {
    File dir = FFat.open(p);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f) {
      String sub = p == "/" ? "/" + String(f.name()) : p + "/" + f.name();
      if (f.isDirectory()) {
        self(sub, self);
        if (!sub.startsWith("/BM")) {
          File check = FFat.open(sub);
          if (check && check.isDirectory()) {
            File child = check.openNextFile();
            if (!child) FFat.rmdir(sub);
          }
        }
      } else {
        if (String(f.name()).endsWith(".bin")) {
          if (FFat.remove(sub)) count++;
          done++;
          if (millis() - lastDraw > 200 && total > 0) {
            lastDraw = millis();
            int pct = (int)(done * 100L / total);
            drawProgressBar(pct, "Deleting tags...", (String(done) + "/" + total).c_str());
            yield();
          }
        }
      }
      f = dir.openNextFile();
    }
  };
  delRec(path, delRec);
}

void apiDeleteAll() {
  int count = 0;
  deleteAllDumpsRecursive("/", count);
  String resp = "{\"ok\":true,\"count\":" + String(count) + "}";
  httpServer.send(200, "application/json", resp);
}

// ── File upload (/api/upload, multipart/form-data, field "file") ──
static File uploadFile;
static bool uploadOk = false;

void apiUploadHandler() {
  HTTPUpload& upload = httpServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    uploadOk = false;
    String fname = upload.filename;
    // Keep only the basename, force .bin extension
    if (fname.lastIndexOf('/') >= 0)
      fname = fname.substring(fname.lastIndexOf('/') + 1);
    if (!fname.endsWith(".bin")) fname += ".bin";
    // Sanitise to plain ASCII
    String safe = "/";
    for (char c : fname)
      safe += (isAlphaNumeric(c) || c == '_' || c == '-' || c == '.') ? c : '_';
    Serial.printf("Upload start: %s\n", safe.c_str());
    DBGF("[UPLOAD] Start: %s\n", safe.c_str());
    uploadFile = FFat.open(safe, FILE_WRITE);
    uploadOk = (bool)uploadFile;

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile)
      uploadFile.write(upload.buf, upload.currentSize);

  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("Upload done: %u bytes\n", upload.totalSize);
      DBGF("[UPLOAD] Done: %u bytes  result=%s\n",
           upload.totalSize, uploadOk ? "OK" : "FAIL");
    }
  }
}

void apiUploadDone() {
  DynamicJsonDocument doc(128);
  doc["success"] = uploadOk;
  doc["message"] = uploadOk ? "File uploaded successfully." : "Upload failed â check filename / FAT space.";
  String out;
  serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

// ── BambuMan catalog sync helpers ────────────────────────────
static bool bmReadExact(WiFiClient* s, uint8_t* buf, int n) {
  int got = 0;
  unsigned long t0 = millis();
  while (got < n) {
    if (!s->connected() && !s->available()) return false;
    int r = s->readBytes(buf + got, n - got);
    if (r > 0) {
      got += r;
      t0 = millis();
    } else if (millis() - t0 > 10000) return false;
    else {
      delay(2);
      yield();
    }
  }
  return true;
}
static void bmSkipBytes(WiFiClient* s, int n) {
  uint8_t tmp[64];
  while (n > 0) {
    int c = min(n, (int)sizeof(tmp));
    if (!bmReadExact(s, tmp, c)) return;
    n -= c;
  }
}

// Decompress raw deflate data from stream, returns true on success
static bool bmInflateToFile(WiFiClient* s, long compSize, long uncompSize, File& outF) {
  static tinfl_decompressor inflator;
  tinfl_init(&inflator);
  static uint8_t inBuf[512];
  static uint8_t outBuf[512];
  long remain = compSize;
  bool done = false;

  while (remain > 0) {
    size_t inLen = min((long)sizeof(inBuf), remain);
    if (!bmReadExact(s, inBuf, inLen)) return false;
    remain -= inLen;

    size_t inOfs = 0;
    while (inOfs < inLen && !done) {
      size_t outLen = sizeof(outBuf);
      size_t inUsed = inLen - inOfs;
      tinfl_status st = tinfl_decompress(&inflator, inBuf + inOfs, &inUsed,
                                          outBuf, outBuf, &outLen, 0);
      if (st < 0) return false;
      inOfs += inUsed;
      if (outLen > 0) outF.write(outBuf, outLen);
      if (st == TINFL_STATUS_DONE) { done = true; break; }
    }
  }
  return done;
}

// Returns URL of today's (or recent) bambuman.ee daily ZIP
String bmFindZipUrl() {
  struct tm t;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  if (!getLocalTime(&t, 8000)) {
    DBGLN("[BM] NTP failed");
    return "";
  }
  for (int i = 0; i < 7; i++) {
    struct tm tt = t;
    tt.tm_mday -= i;
    mktime(&tt);
    char url[80];
    snprintf(url, sizeof(url),
             "https://bambuman.ee/files/data_%04d-%02d-%02d.zip",
             tt.tm_year + 1900, tt.tm_mon + 1, tt.tm_mday);
    HTTPClient hc;
    hc.begin(url);
    hc.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64) BambuTagger/1.0");
    int code = hc.sendRequest("HEAD");
    hc.end();
    DBGF("[BM] probe %s -> %d\n", url, code);
    if (code == 200) return String(url);
  }
  return "";
}

void bmCacheInvalidate() {
  bmCL0n = 0; bmCL1n = 0; bmCL2n = 0;
  bmCacheValid = false;
  DBGLN("[BM] Cache invalidated");
}

// POST /api/bm/sync – download full ZIP, extract data.bin files, build catalog
void apiBmSync() {
  if (!WiFi.isConnected()) {
    httpServer.send(503, "application/json", "{\"error\":\"No WiFi\"}");
    return;
  }

  // Show status on display
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(3);
  lcd.setCursor(10, 138); lcd.print("Syncing...");
  drawFooter(); lcd.display();
  ledSet(0, 0, 80);

  String zipUrl = bmFindZipUrl();
  if (zipUrl.isEmpty()) {
    lcd.setCursor(10, 166); lcd.print("ZIP not found");
    httpServer.send(503, "application/json", "{\"error\":\"ZIP URL not found\"}");
    delay(3000);
    return;
  }

  // Ensure fresh catalog
  if (!FFat.exists("/BM")) FFat.mkdir("/BM");
  FFat.remove("/BM/catalog.json");

  File catF = FFat.open("/BM/catalog.json", "w");
  if (!catF) {
    httpServer.send(503, "application/json", "{\"error\":\"Cannot write catalog\"}");
    return;
  }
  catF.print("[");

  int count = 0;
  bool firstCat = true;
  unsigned long lastDraw = 0;

  WiFiClientSecure wcs;
  wcs.setInsecure();
  HTTPClient hc;
  hc.begin(wcs, zipUrl);
  hc.addHeader("User-Agent", "Mozilla/5.0 (compatible; BambuTagger/1.0; ESP32)");
  int code = hc.GET();
  if (code != 200) {
    catF.print("]"); catF.close(); hc.end();
    lcd.setCursor(10, 166); lcd.print("HTTP " + String(code));
    httpServer.send(503, "application/json", "{\"error\":\"HTTP " + String(code) + "\"}");
    delay(3000);
    return;
  }

  WiFiClient* stream = hc.getStreamPtr();
  uint8_t lhdr[26];
  uint8_t fname[280];

  while (true) {
    uint32_t sig = 0;
    if (!bmReadExact(stream, (uint8_t*)&sig, 4)) break;
    if (sig != 0x04034b50) break;

    if (!bmReadExact(stream, lhdr, 26)) break;

    uint16_t compMethod = lhdr[4] | ((uint16_t)lhdr[5] << 8);
    uint32_t compSize   = (uint32_t)lhdr[14] | ((uint32_t)lhdr[15] << 8)
                        | ((uint32_t)lhdr[16] << 16) | ((uint32_t)lhdr[17] << 24);
    uint32_t uncompSize = (uint32_t)lhdr[18] | ((uint32_t)lhdr[19] << 8)
                        | ((uint32_t)lhdr[20] << 16) | ((uint32_t)lhdr[21] << 24);
    uint16_t fnLen = lhdr[22] | ((uint16_t)lhdr[23] << 8);
    uint16_t exLen = lhdr[24] | ((uint16_t)lhdr[25] << 8);

    int fnRead = min((int)fnLen, 279);
    if (!bmReadExact(stream, fname, fnRead)) break;
    fname[fnRead] = 0;
    if (fnLen > fnRead) bmSkipBytes(stream, fnLen - fnRead);
    if (exLen > 0) bmSkipBytes(stream, exLen);

    String path = String((char*)fname);

    // Update display periodically
    if (millis() - lastDraw > 500) {
      lastDraw = millis();
      lcd.fillScreen(TFT_BLACK);
      drawStatusBar(); drawSubHeader("BambuMan Sync");
      lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(3);
      lcd.setCursor(10, 138);
      lcd.print("Extracting...");
      lcd.setCursor(10, 166);
      lcd.print(String(count) + " files");
      drawFooter(); lcd.display();
      yield();
    }

    if (!path.endsWith("/data.bin")) {
      bmSkipBytes(stream, compSize);
      continue;
    }

    int s0 = path.indexOf('/');
    int s1 = s0 >= 0 ? path.indexOf('/', s0 + 1) : -1;
    int s2 = s1 >= 0 ? path.indexOf('/', s1 + 1) : -1;
    int s3 = s2 >= 0 ? path.indexOf('/', s2 + 1) : -1;
    if (s0 < 0 || s1 < 0 || s2 < 0 || s3 < 0) {
      bmSkipBytes(stream, compSize);
      continue;
    }
    String mat = path.substring(0, s0);
    String typ = path.substring(s0 + 1, s1);
    String col = path.substring(s1 + 1, s2);
    String uid = path.substring(s2 + 1, s3);

    mat.replace("\"", "\\"); typ.replace("\"", "\\");
    col.replace("\"", "\\"); uid.replace("\"", "\\");

    if (!firstCat) catF.print(",");
    firstCat = false;
    catF.print("{\"u\":\""); catF.print(uid);
    catF.print("\",\"m\":\""); catF.print(mat);
    catF.print("\",\"t\":\""); catF.print(typ);
    catF.print("\",\"c\":\""); catF.print(col);
    catF.print("\"}");

    // Build directory and extract file
    String dir = "/" + mat;
    if (!FFat.exists(dir)) FFat.mkdir(dir);
    dir += "/" + typ;
    if (!FFat.exists(dir)) FFat.mkdir(dir);
    dir += "/" + col;
    if (!FFat.exists(dir)) FFat.mkdir(dir);

    String filePath = dir + "/" + uid + ".bin";
    File outF = FFat.open(filePath, "w");
    if (outF) {
      bool ok = false;
      if (compMethod == 0 && compSize == DUMP_SIZE) {
        uint8_t buf[256];
        long remain = compSize;
        ok = true;
        while (remain > 0) {
          int chunk = min((long)sizeof(buf), remain);
          if (!bmReadExact(stream, buf, chunk)) { ok = false; break; }
          outF.write(buf, chunk);
          remain -= chunk;
        }
      } else if (compMethod == 8) {
        ok = bmInflateToFile(stream, compSize, uncompSize, outF);
      } else {
        bmSkipBytes(stream, compSize);
      }
      outF.close();
      if (ok) count++;
      else FFat.remove(filePath);
    } else {
      bmSkipBytes(stream, compSize);
    }
    if (count % 50 == 0) yield();
  }

  catF.print("]");
  catF.flush();
  catF.close();
  hc.end();
  DBGF("[BM] Catalog: %d files\n", count);
  bmCacheInvalidate();

  // Done screen on display
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(3);
  lcd.setCursor(10, 138); lcd.print("Done!");
  lcd.setCursor(10, 166); lcd.print(String(count) + " files");
  drawFooter(); lcd.display();
  ledSet(0, 0, 40);

  String resp = "{\"ok\":true,\"count\":" + String(count) + "}";
  httpServer.send(200, "application/json", resp);
}

// GET /api/bm/catalog – serve /BM/catalog.json
void apiBmCatalog() {
  if (!FFat.exists("/BM/catalog.json")) {
    httpServer.send(404, "application/json", "{\"error\":\"Not synced yet\"}");
    return;
  }
  File f = FFat.open("/BM/catalog.json", "r");
  if (!f) {
    httpServer.send(500, "application/json", "{\"error\":\"Open failed\"}");
    return;
  }
  httpServer.streamFile(f, "application/json");
  f.close();
}

// ── BambuMan per-tag download (/api/bm/fetch?uid=XXXXXXXX) ───
void apiBmFetch() {
  String uid = httpServer.arg("uid");
  uid.trim();
  uid.toUpperCase();
  DBGF("[HTTP]  GET /api/bm/fetch  uid=%s\n", uid.c_str());

  DynamicJsonDocument resp(256);
  auto fail = [&](int hcode, const char* msg) {
    resp["ok"] = false;
    resp["error"] = msg;
    String out;
    serializeJson(resp, out);
    httpServer.send(hcode, "application/json", out);
  };

  if (uid.length() < 4) {
    fail(400, "uid param required (min 4 hex chars)");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    fail(503, "WiFi not connected");
    return;
  }

  String url = "https://bambuman.ee/dl/tags/" + uid + "/data.bin";
  DBGF("[BM]  Fetching %s\n", url.c_str());

  WiFiClientSecure wcs;
  wcs.setInsecure();
  HTTPClient http;
  http.begin(wcs, url);
  http.addHeader("User-Agent", "Mozilla/5.0 (compatible; BambuTagger/1.0; ESP32)");
  http.addHeader("Accept", "application/octet-stream");
  int code = http.GET();

  if (code != 200) {
    DBGF("[BM]  HTTP %d\n", code);
    const char* msg = code == 404   ? "UID not found on bambuman.ee"
                      : code == 403 ? "Blocked by Cloudflare (try with your browser)"
                                    : ("bambuman.ee HTTP " + String(code)).c_str();
    fail(code == 404 ? 404 : 502, msg);
    http.end();
    return;
  }

  int totalSize = http.getSize();
  if (totalSize > 0 && totalSize != DUMP_SIZE) {
    fail(422, ("Unexpected file size: " + String(totalSize)).c_str());
    http.end();
    return;
  }

  // ── Resolve save path (structured) ──────────────────────────────────────
  String mat = httpServer.arg("mat");
  String typ = httpServer.arg("type");
  String col = httpServer.arg("color");
  mat.trim();
  typ.trim();
  col.trim();
  // If m/t/c not supplied, try catalog lookup
  if (mat.isEmpty() || typ.isEmpty() || col.isEmpty()) {
    String lm, lt, lc;
    if (bmLookupCatalog(uid, lm, lt, lc)) {
      if (mat.isEmpty()) mat = lm;
      if (typ.isEmpty()) typ = lt;
      if (col.isEmpty()) col = lc;
    }
  }
  String savePath;
  if (!mat.isEmpty() && !typ.isEmpty() && !col.isEmpty()) {
    savePath = buildBmFilePath(mat, typ, col, uid);
  } else {
    if (!FFat.exists("/BM")) FFat.mkdir("/BM");
    savePath = "/BM/" + uid + ".bin";
    DBGLN("[BM]  No m/t/c — using fallback path");
  }
  ensureParentDirs(savePath);
  File f = FFat.open(savePath, "w");
  if (!f) {
    fail(500, "FFat open failed");
    http.end();
    return;
  }

  int written = http.writeToStream(&f);
  f.close();
  http.end();

  if (written != DUMP_SIZE) {
    FFat.remove(savePath);
    fail(500, ("Incomplete write: " + String(written) + "/" + String(DUMP_SIZE)).c_str());
    return;
  }

  bmIndexAdd(savePath);
  DBGF("[BM]  Saved %s (%d bytes)\n", savePath.c_str(), written);
  resp["ok"] = true;
  resp["path"] = savePath;
  resp["size"] = written;
  String out;
  serializeJson(resp, out);
  httpServer.send(200, "application/json", out);
}

// ── Write tag from FAT dump via REST (/api/writetag) ───────────────────────
void apiWriteTag() {
  DBGLN("[HTTP]  POST /api/writetag");
  DynamicJsonDocument req(256);
  deserializeJson(req, httpServer.arg("plain"));
  String path = req["path"] | "";
  DynamicJsonDocument resp(256);
  if (path.isEmpty()) {
    resp["ok"] = false;
    resp["message"] = "path required";
    String out;
    serializeJson(resp, out);
    httpServer.send(400, "application/json", out);
    return;
  }
  File f = FFat.open(path);
  if (!f || f.size() != DUMP_SIZE) {
    if (f) f.close();
    resp["ok"] = false;
    resp["message"] = "File not found or wrong size";
    String out;
    serializeJson(resp, out);
    httpServer.send(404, "application/json", out);
    return;
  }
  f.read(dumpBuf, DUMP_SIZE);
  f.close();
  strncpy(selectedDumpPath, path.c_str(), sizeof(selectedDumpPath) - 1);
  selectedDumpPath[sizeof(selectedDumpPath) - 1] = '\0';
  g_webWrite = true;           // flag: show OLED progress during write
  appState = S_DUMP_WRITE;
  resp["ok"] = true;
  resp["message"] = "Place tag on RFID reader within 20 s";
  String out;
  serializeJson(resp, out);
  httpServer.send(200, "application/json", out);
}

// ──────────────────────────────────────────────────────────────
//  OTA firmware update  (OLED-driven + web API)
// ──────────────────────────────────────────────────────────────

struct OtaRelease {
  String tag;    // e.g. "v1.0.1"
  String dlUrl;  // HTTPS download URL for the app .bin asset
  int    size;   // bytes, 0 = unknown
  bool   ok;     // false = API error / no valid asset
};

// Fetch latest release metadata from GitHub
OtaRelease ghGetLatestRelease() {
  OtaRelease rel; rel.ok = false; rel.size = 0;
  if (WiFi.status() != WL_CONNECTED) return rel;

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://api.github.com/repos/" OTA_REPO "/releases/latest");
  ghAddHeaders(http);
  int code = http.GET();
  DBGF("[OTA]  releases/latest → HTTP %d\n", code);
  if (code != 200) { http.end(); return rel; }

  // Filter to keep only needed fields — avoids loading the full JSON body
  StaticJsonDocument<96> filter;
  filter["tag_name"] = true;
  JsonArray fa = filter.createNestedArray("assets");
  JsonObject fa0 = fa.createNestedObject();
  fa0["name"]                 = true;
  fa0["browser_download_url"] = true;
  fa0["size"]                 = true;

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { DBGF("[OTA]  JSON parse: %s\n", err.c_str()); return rel; }

  rel.tag = doc["tag_name"] | "";
  // Normalise: strip leading 'v' so "v1.6.0" and "1.6.0" compare equal
  if (rel.tag.startsWith("v") || rel.tag.startsWith("V"))
    rel.tag = rel.tag.substring(1);

  JsonArray assets = doc["assets"].as<JsonArray>();
  DBGF("[OTA]  tag=%s assets=%d\n", rel.tag.c_str(), (int)assets.size());

  for (JsonObject asset : assets) {
    String name = asset["name"] | "";
    DBGF("[OTA]  candidate asset: %s\n", name.c_str());
    // App binary: ends .bin, not merged / bootloader / partitions / elf
    if (name.endsWith(".bin") &&
        name.indexOf("merged")      < 0 &&
        name.indexOf("bootloader")  < 0 &&
        name.indexOf("partition")   < 0) {
      rel.dlUrl = asset["browser_download_url"] | "";
      rel.size  = asset["size"] | 0;
      rel.ok    = true;
      DBGF("[OTA]  chosen: %s  size=%d\n", name.c_str(), rel.size);
      break;
    }
  }
  if (!rel.ok)
    rel.tag = rel.tag + " (no asset)";  // tag carries the hint
  return rel;
}

// Draw a progress bar on the touchscreen (pct 0-100)
void drawProgressBar(int pct, const char* phase, const char* label) {
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader(phase);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(3);
  lcd.setCursor(10, 125); lcd.print(label);
  // Big progress bar
  int barW = LCD_WIDTH - 40, barH = 40, barX = 20, barY = 200;
  lcd.drawRoundRect(barX, barY, barW, barH, 4, TFT_WHITE);
  int fill = (int)((long)pct * (barW - 4) / 100);
  if (fill > 0) lcd.fillRoundRect(barX + 2, barY + 2, fill, barH - 4, 3, TFT_BLUE);
  char pctStr[16]; snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setCursor(LCD_WIDTH / 2 - 20, barY + barH + 15);
  lcd.print(pctStr);
  drawFooter(); lcd.display();
}

void otaDrawProgress(int pct, const char* label) {
  drawProgressBar(pct, "OTA Firmware", label);
}

void drawWriteScreen(const char* phase, int sectDone, int sectTotal) {
  int pct = (sectTotal > 0) ? (sectDone * 100 / sectTotal) : 0;
  char info[24];
  snprintf(info, sizeof(info), "%d / %d sec", sectDone, sectTotal);
  drawProgressBar(pct, "Write Tag", info);
}

// Sector-progress callback — called by rfidWriteDump() after each sector
static void writeProgressCbFn(int done, int total) {
  drawWriteScreen(g_webWrite ? "Web: writing..." : "writing...", done, total);
}

// Internal OTA flash — used by both OLED flow and web API
// Returns empty string on success, error message on failure
String otaFlash(const OtaRelease& rel, bool progressOled) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, rel.dlUrl);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ghAddHeaders(http);
  int code = http.GET();
  DBGF("[OTA]  flash GET → HTTP %d\n", code);
  if (code != 200) {
    http.end();
    return "HTTP " + String(code);
  }

  int totalSize = (rel.size > 0) ? rel.size : http.getSize();
  DBGF("[OTA]  totalSize=%d\n", totalSize);
  if (!Update.begin((totalSize > 0) ? (size_t)totalSize : UPDATE_SIZE_UNKNOWN)) {
    String e = Update.errorString();
    http.end();
    return "begin: " + e;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t  buf[512];
  int      written   = 0;
  unsigned long lastDraw = 0;

  while (http.connected() && (totalSize <= 0 || written < totalSize)) {
    int avail = stream->available();
    if (!avail) { delay(2); continue; }
    int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
    if (n <= 0) break;
    if (Update.write(buf, n) != (size_t)n) {
      String e = Update.errorString();
      http.end(); Update.abort();
      return "write: " + e;
    }
    written += n;
    if (progressOled && millis() - lastDraw > 200) {
      int pct = (totalSize > 0) ? (written * 100 / totalSize) : 50;
      otaDrawProgress(pct, "Flashing...");
      lastDraw = millis();
    }
  }
  http.end();

  if (!Update.end(true)) {
    return "end: " + String(Update.errorString());
  }
  DBGF("[OTA]  flash OK — %d bytes written\n", written);
  return "";  // success
}

void enterMainMenu() {
  DBGLN("[STATE] -> MAIN_MENU");
  appState = S_MAIN_MENU;
  ledOff();
  drawMenu();
}

// forward declaration (defined below with apiOtaCheck)
static bool semverGt(const String& a, const String& b);

// OLED-driven blocking OTA flow
void processOtaUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    showStatus("OTA Update\nNo WiFi!");
    ledFlash(255, 80, 0, 2);
    appState = S_WIFI_INFO;
    return;
  }

  // 1/3 — Check latest release
  ledSet(0, 0, 180);
  showStatus("OTA Update\n\n1/3 Checking\nGitHub...");
  OtaRelease rel = ghGetLatestRelease();

  if (!rel.ok) {
    // rel.tag carries a hint when available (e.g. "1.6.0 (no asset)")
    String hint = rel.tag.length() ? rel.tag : "See serial log";
    showStatus(("OTA Update\nCheck failed!\n" + hint + "").c_str());
    ledFlash(255, 0, 0, 2);
    appState = S_WIFI_INFO;
    return;
  }

  // 2/3 — Version compare (rel.tag is already stripped of leading 'v')
  String current = FIRMWARE_VERSION;  // bare e.g. "1.6.0"
  if (!semverGt(current, rel.tag)) {
    // latest is equal to or older than running firmware
    String msg = (rel.tag == current)
      ? ("OTA Update\n\nUp to date!\nv" + current + "")
      : ("OTA Update\n\nNo newer release\nLatest: v" + rel.tag +
         "\nRunning: v" + current + "");
    showStatus(("OTA Update\n" + msg).c_str());
    ledFlash(0, 255, 0, 2);
    appState = S_WIFI_INFO;
    return;
  }

  // 3/3 — Prompt
  String prompt = "Update!\nNow: v" + current +
                  "\nNew: v" + rel.tag;
  showStatus(("OTA Update\n" + prompt).c_str());
  ledSet(0, 80, 200);

  unsigned long t0 = millis();
  while (millis() - t0 < 30000) {
    httpServer.handleClient();
    if (touchPoll()) break;
    delay(10);
  }
  if (millis() - t0 >= 30000) { enterMainMenu(); return; }

  // Flash
  otaDrawProgress(0, "Starting...");
  ledSet(255, 200, 0);

  String err = otaFlash(rel, true);
  if (!err.isEmpty()) {
    showStatus(("OTA Update\nOTA Failed!\n" + err + "").c_str());
    ledFlash(255, 0, 0, 3);
    appState = S_WIFI_INFO;
    return;
  }

  otaDrawProgress(100, "Done! Rebooting");
  ledFlash(0, 255, 0, 3);
  DBGLN("[OTA]  Update complete — rebooting");
  delay(2000);
  ESP.restart();
}

// Compare two bare semver strings (no leading 'v').  Returns true if b > a.
static bool semverGt(const String& a, const String& b) {
  // Parse up to 3 dot-separated numeric components
  auto parse = [](const String& s, int out[3]) {
    int i = 0, comp = 0;
    out[0] = out[1] = out[2] = 0;
    for (char c : s) {
      if (c == '.') { if (++comp >= 3) break; }
      else if (c >= '0' && c <= '9') out[comp] = out[comp] * 10 + (c - '0');
    }
  };
  int va[3], vb[3];
  parse(a, va); parse(b, vb);
  for (int i = 0; i < 3; i++) {
    if (vb[i] > va[i]) return true;
    if (vb[i] < va[i]) return false;
  }
  return false; // equal
}

// GET /api/ota/check  — returns current + latest version info
void apiOtaCheck() {
  DBGLN("[HTTP]  GET /api/ota/check");
  DynamicJsonDocument doc(512);
  doc["current"] = "v" FIRMWARE_VERSION;

  if (WiFi.status() != WL_CONNECTED) {
    doc["ok"]    = false;
    doc["error"] = "No WiFi";
  } else {
    OtaRelease rel = ghGetLatestRelease();
    if (!rel.ok) {
      // rel.tag carries hint if release was found but had no asset
      doc["ok"]    = false;
      doc["error"] = rel.tag.length() ? rel.tag : "GitHub API error";
    } else {
      // FIRMWARE_VERSION may or may not carry 'v' — normalise both sides
      String fwNorm = FIRMWARE_VERSION;
      if (fwNorm.startsWith("v") || fwNorm.startsWith("V"))
        fwNorm = fwNorm.substring(1);
      doc["ok"]               = true;
      doc["latest"]           = rel.tag;
      doc["download_url"]     = rel.dlUrl;
      doc["size"]             = rel.size;
      doc["update_available"] = semverGt(fwNorm, rel.tag);
    }
  }
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

// POST /api/ota/update  — download and flash, then reboot
void apiOtaUpdate() {
  DBGLN("[HTTP]  POST /api/ota/update");

  // ── OLED: checking ──────────────────────────────────────────
  otaDrawProgress(0, "Web: checking...");

  OtaRelease rel = ghGetLatestRelease();
  if (!rel.ok) {
    // OLED: show failure hint
    String hint = rel.tag.length() ? rel.tag : "See serial log";
    showStatus(("OTA Update\nOTA Web\n\nCheck failed!\n" + hint).c_str());
    httpServer.send(503, "application/json",
                    "{\"ok\":false,\"error\":\"Could not fetch release info\"}");
    return;
  }

  // ── OLED: show target version and start progress bar ────────
  String label = "Web: v" + rel.tag;
  otaDrawProgress(0, label.c_str());
  ledSet(255, 200, 0);

  String err = otaFlash(rel, true);   // true → OLED progress during flash
  if (!err.isEmpty()) {
    // OLED: show error (stays visible until next user action)
    showStatus(("OTA Update\nOTA Failed!\n" + err).c_str());
    ledFlash(255, 0, 0, 3);
    DynamicJsonDocument doc(256);
    doc["ok"] = false; doc["error"] = err;
    String out; serializeJson(doc, out);
    httpServer.send(500, "application/json", out);
    return;
  }

  // ── OLED: success ────────────────────────────────────────────
  otaDrawProgress(100, "Done! Rebooting");
  ledFlash(0, 255, 0, 3);
  httpServer.send(200, "application/json", "{\"ok\":true}");
  DBGLN("[OTA]  Web-triggered update complete — rebooting");
  delay(1500);
  ESP.restart();
}

void setupHTTPServer() {
  httpServer.on("/", HTTP_GET, []() {
    httpServer.send_P(200, "text/html", INDEX_HTML);
  });
  httpServer.on("/api/status", HTTP_GET, apiStatus);
  httpServer.on("/api/wifi", HTTP_POST, apiWifi);
  httpServer.on("/api/token", HTTP_GET, apiTokenGet);
  httpServer.on("/api/token", HTTP_POST, apiTokenSet);
  httpServer.on("/api/scan", HTTP_GET, apiScan);
  httpServer.on("/api/list", HTTP_GET, apiList);
  httpServer.on("/api/download", HTTP_POST, apiDownload);
  httpServer.on("/api/files", HTTP_GET, apiFiles);
  httpServer.on("/api/delete", HTTP_POST, apiDelete);
  httpServer.on("/api/deleteall", HTTP_POST, apiDeleteAll);
  httpServer.on("/api/upload", HTTP_POST, apiUploadDone, apiUploadHandler);
  httpServer.on("/api/writetag", HTTP_POST, apiWriteTag);
  httpServer.on("/api/bm/fetch", HTTP_GET, apiBmFetch);
  httpServer.on("/api/bm/list", HTTP_GET, apiBmList);
  httpServer.on("/api/bm/sync", HTTP_POST, apiBmSync);
  httpServer.on("/api/bm/catalog", HTTP_GET,  apiBmCatalog);
  httpServer.on("/api/ota/check",  HTTP_GET,  apiOtaCheck);
  httpServer.on("/api/ota/update", HTTP_POST, apiOtaUpdate);
  httpServer.enableCORS(true);
  httpServer.begin();
  Serial.println("HTTP server started.");
}

// ──────────────────────────────────────────────────────────────
//  FAT dump file list helpers
// ──────────────────────────────────────────────────────────────
// ── FAT directory browser helpers ─────────────────────────────
// Extract last path segment: "/PLA/BLACK/3AD.bin" -> "3AD.bin"
static String fatLastSeg(const char* fp) {
  String s(fp);
  int sl = s.lastIndexOf('/');
  return (sl >= 0) ? s.substring(sl + 1) : s;
}

// Load one directory level into fatEntries[]: dirs first, then .bin files.
void fatLoadDir(const String& path) {
  fatCount = 0;
  fatSel = (fatDepth > 0) ? 0 : 0;  // 0 always; <BACK is virtual row 0
  fatScroll = 0;
  String p = path.isEmpty() ? "/" : path;

  // Pass 1 – subdirectories
  File dir = FFat.open(p);
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f && fatCount < FAT_MAX_ENTRIES) {
    if (f.isDirectory()) {
      String seg = fatLastSeg(f.name());
      if (!seg.endsWith("BM")) {
        strncpy(fatEntries[fatCount].name, seg.c_str(), 47);
        fatEntries[fatCount].name[47] = '\0';
        fatEntries[fatCount].isDir = true;
        fatCount++;
      }
    }
    f = dir.openNextFile();
  }
  dir.close();

  // Pass 2 – .bin files
  dir = FFat.open(p);
  f = dir.openNextFile();
  while (f && fatCount < FAT_MAX_ENTRIES) {
    String n(f.name());
    if (!f.isDirectory() && (n.endsWith(".bin") || fatLastSeg(f.name()).endsWith(".bin"))) {
      String seg = fatLastSeg(f.name());
      strncpy(fatEntries[fatCount].name, seg.c_str(), 47);
      fatEntries[fatCount].name[47] = '\0';
      fatEntries[fatCount].isDir = false;
      fatCount++;
    }
    f = dir.openNextFile();
  }
  dir.close();
}

// Navigate browser state to the parent dir of filePath and pre-select it.
// Does NOT redraw – caller does that.
void fatNavigateTo(const String& filePath) {
  int last = filePath.lastIndexOf('/');
  String par = (last > 0) ? filePath.substring(0, last) : "/";
  if (par.isEmpty()) par = "/";

  fatDepth = 0;
  fatCurPath = "/";

  if (par != "/") {
    // Walk segments to build ancestor stack
    for (int i = 1; i <= (int)par.length(); i++) {
      if (i == (int)par.length() || par[i] == '/') {
        if (fatDepth < FAT_MAX_DEPTH) fatDirStack[fatDepth++] = fatCurPath;
        fatCurPath = par.substring(0, i);
      }
    }
  }
  fatLoadDir(fatCurPath);

  // Try to pre-select the downloaded file
  String target = filePath.substring(last + 1);
  bool hasBack = (fatDepth > 0);
  for (int i = 0; i < fatCount; i++) {
    if (!fatEntries[i].isDir && String(fatEntries[i].name) == target) {
      fatSel = i + (hasBack ? 1 : 0);
      break;
    }
  }
}

// ──────────────────────────────────────────────────────────────
//  State-machine entry points
// ──────────────────────────────────────────────────────────────

// ══════════════════════════════════════════════════════════════
//  GitHub OLED browser
// ══════════════════════════════════════════════════════════════

// Fetch one directory level from the GitHub Contents API.
// Fills ghEntries[] / ghCount.  Returns true on success.
bool ghFetchDir(const String& repoPath) {
  if (WiFi.status() != WL_CONNECTED) {
    DBGLN("[GH]  No WiFi – cannot browse");
    return false;
  }
  String url = "https://" GITHUB_API_HOST "/repos" GITHUB_REPO_PATH "/contents/";
  String repoPathTmp = repoPath;
  repoPathTmp.replace(" ", "%20");
  if (!repoPath.isEmpty()) url += repoPathTmp;

  DBGF("[GH]  Fetching: %s\n", url.c_str());

  // Retry up to 2 times
  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    ghAddHeaders(http);
    http.setTimeout(40000);
    int code = http.GET();
    if (code == 200) {
      // Parse – only keep name, path, type
      StaticJsonDocument<256> filter;
      filter[0]["name"] = true;
      filter[0]["path"] = true;
      filter[0]["type"] = true;

      DynamicJsonDocument* doc = new DynamicJsonDocument(64000);
      DeserializationError err = deserializeJson(*doc, http.getStream(),
                                                  DeserializationOption::Filter(filter));
      http.end();
      if (!err) {
        ghCount = 0;
        for (JsonObject item : doc->as<JsonArray>()) {
          if (ghCount >= GH_MAX_ENTRIES) break;
          String name = item["name"] | "";
          String path = item["path"] | "";
          String type = item["type"] | "";
          if (type == "file" && !name.endsWith(".json") && !name.endsWith(".bin")) continue;
          if (name.startsWith(".")) continue;
          strncpy(ghEntries[ghCount].name, name.c_str(), 47);
          ghEntries[ghCount].name[47] = '\0';
          strncpy(ghEntries[ghCount].path, path.c_str(), 127);
          ghEntries[ghCount].path[127] = '\0';
          ghEntries[ghCount].isDir = (type == "dir");
          ghCount++;
        }
        DBGF("[GH]  Loaded %d entries for '%s'\n", ghCount, repoPath.c_str());
        delete doc;
        return true;
      }
      DBGF("[GH]  JSON error: %s\n", err.c_str());
      delete doc;
      return false;
    }
    DBGF("[GH]  HTTP error %d (attempt %d)\n", code, attempt + 1);
    http.end();
    if (attempt < 1) delay(1000);
  }
  return false;
}

// Parse a GitHub dump.json into raw MIFARE binary (DUMP_SIZE bytes).
// Supports:
//   - Array of 64 hex strings (16 chars each)
//   - Object {"0":"hex...","1":"hex...",...}
//   - Object {"blocks": <above>}
//   - Object {"Cards":[{"Blocks":{...}}]}
// Returns number of bytes written (DUMP_SIZE on success, 0 on failure).
int ghParseJson(const uint8_t* jsonBytes, size_t jsonLen, uint8_t* outBuf) {
  memset(outBuf, 0xFF, DUMP_SIZE);

  DynamicJsonDocument doc(32768);
  DeserializationError err = deserializeJson(doc, jsonBytes, jsonLen);
  if (err) {
    DBGF("[GH]  JSON parse error: %s\n", err.c_str());
    return 0;
  }

  auto hexToBin = [](const char* hex, uint8_t* dst, int len) -> bool {
    for (int i = 0; i < len; i++) {
      char hi = hex[i * 2], lo = hex[i * 2 + 1];
      if (!isxdigit(hi) || !isxdigit(lo)) return false;
      auto h2n = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        return 10 + c - 'A';
      };
      dst[i] = (h2n(hi) << 4) | h2n(lo);
    }
    return true;
  };

  int found = 0;

  // Try: plain array of 64 hex strings
  if (doc.is<JsonArray>()) {
    JsonArray arr = doc.as<JsonArray>();
    int blk = 0;
    for (JsonVariant v : arr) {
      if (blk >= 64) break;
      const char* hex = v.as<const char*>();
      if (hex && strlen(hex) >= 32) {
        hexToBin(hex, outBuf + blk * 16, 16);
        found++;
      }
      blk++;
    }
    if (found == 64) return DUMP_SIZE;
  }

  // Try: top-level or nested "blocks"/"Blocks" object {"0":"hex",...}
  JsonVariant blocks;                                  //doc['blocks'] | doc['Blocks'] | doc['Cards'][0]['Blocks'] | doc['Cards'][0]['blocks'];
  if (blocks.isNull()) blocks = doc.as<JsonObject>();  // try root as blocks

  if (blocks.is<JsonObject>()) {
    for (JsonPair kv : blocks.as<JsonObject>()) {
      int blk = atoi(kv.key().c_str());
      if (blk < 0 || blk >= 64) continue;
      const char* hex = kv.value().as<const char*>();
      if (!hex) {
        // Might be array ["AB CD EF..."]
        if (kv.value().is<JsonArray>()) {
          String joined = "";
          for (JsonVariant b : kv.value().as<JsonArray>())
            joined += String(b.as<const char*>() ? b.as<const char*>() : "");
          joined.replace(" ", "");
          if (joined.length() >= 32) {
            hexToBin(joined.c_str(), outBuf + blk * 16, 16);
            found++;
          }
        }
        continue;
      }
      // Strip spaces
      String h = String(hex);
      h.replace(" ", "");
      if (h.length() >= 32) {
        hexToBin(h.c_str(), outBuf + blk * 16, 16);
        found++;
      }
    }
    if (found > 0) return DUMP_SIZE;
  }

  DBGF("[GH]  JSON: could not find block data (found=%d)\n", found);
  return 0;
}

// Download a raw URL and save to FFat.  If it's a JSON file, parse it to
// binary first and save as .bin.  Returns true on success.
bool ghSaveFile(const String& rawUrl, const String& localName) {
  DBGF("[GH]  Downloading: %s -> %s\n", rawUrl.c_str(), localName.c_str());

  String rawUrlTmp = rawUrl;
  rawUrlTmp.replace(" ", "%20");

  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, rawUrlTmp);
    ghAddHeaders(http);
    http.setTimeout(45000);
    int code = http.GET();
    if (code != 200) {
      DBGF("[GH]  HTTP error %d (attempt %d)\n", code, attempt + 1);
      http.end();
      if (attempt < 1) delay(1000);
      continue;
    }

  int size = http.getSize();
  bool isJson = rawUrl.endsWith(".json");

  if (isJson) {
    // Buffer the JSON (max 32 KB) then convert to binary
    const int MAX_JSON = 32768;
    uint8_t* jbuf = (uint8_t*)malloc(MAX_JSON);
    if (!jbuf) {
      http.end();
      return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    int got = 0;
    unsigned long t0 = millis();
    while (got < MAX_JSON - 1 && millis() - t0 < 12000) {
      int avail = stream->available();
      if (avail > 0) {
        int n = stream->readBytes(jbuf + got,
                                  min(avail, MAX_JSON - 1 - got));
        got += n;
      } else if (!http.connected()) break;
      else delay(1);
    }
    http.end();
    jbuf[got] = '\0';
    DBGF("[GH]  JSON bytes read: %d\n", got);

    uint8_t binBuf[DUMP_SIZE];
    int converted = ghParseJson(jbuf, got, binBuf);
    free(jbuf);

    if (converted != DUMP_SIZE) {
      DBGLN("[GH]  JSON→bin conversion failed");
      return false;
    }

    // Save binary
    String savePath = localName;
    if (!savePath.startsWith("/")) savePath = "/" + savePath;
    if (!savePath.endsWith(".bin")) savePath += ".bin";
    ensureParentDirs(savePath);
    File f = FFat.open(savePath, FILE_WRITE);
    if (!f) return false;
    f.write(binBuf, DUMP_SIZE);
    f.close();
    DBGF("[GH]  Saved binary: %s\n", savePath.c_str());
    return true;

  } else {
    // Raw binary download
    if (size > 0 && size != DUMP_SIZE) {
      DBGF("[GH]  Unexpected size %d\n", size);
      http.end();
      return false;
    }
    String savePath = localName;
    if (!savePath.startsWith("/")) savePath = "/" + savePath;
    if (!savePath.endsWith(".bin")) savePath += ".bin";
    ensureParentDirs(savePath);
    File f = FFat.open(savePath, FILE_WRITE);
    if (!f) {
      http.end();
      return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[128];
    int written = 0;
    unsigned long t0 = millis();
    while (written < DUMP_SIZE && millis() - t0 < 40000) {
      int avail = stream->available();
      if (avail > 0) {
        int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
        f.write(buf, n);
        written += n;
      } else if (!http.connected()) break;
      else delay(1);
    }
    f.close();
    http.end();
    if (written != DUMP_SIZE) {
      FFat.remove(savePath);
      DBGF("[GH]  Incomplete: %d/%d\n", written, DUMP_SIZE);
      return false;
    }
    DBGF("[GH]  Saved: %s (%d bytes)\n", savePath.c_str(), written);
    return true;
  }
  }
  return false;
}

// Draw the GitHub browser screen
void drawGhBrowser() {
  lcd.fillScreen(TFT_BLACK);

  drawStatusBar(); drawSubHeader("GitHub Library");

  // Breadcrumb
  if (ghDepth > 0) {
    lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK); lcd.setTextSize(2);
    lcd.setCursor(12, 120);
    String crumb = ghStack[ghDepth - 1];
    if (crumb.length() > 44) crumb = "..." + crumb.substring(crumb.length() - 41);
    lcd.print(crumb);
  }

  int headerRows = (ghDepth == 0) ? 1 : 2;
  int totalRows  = ghCount + headerRows;

  if (totalRows == headerRows) {
    lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(3);
    lcd.setCursor(10, 138); lcd.print("(empty)");
    return;
  }

  for (int row = 0; row < LIST_MAX_VIS; row++) {
    int idx = ghScroll + row;
    if (idx >= totalRows) break;

    int y = LIST_ROW_Y0 + row * LIST_ROW_H;
    bool sel = (idx == ghSel);

    String label;
    if (idx == 0) {
      lcd.setTextSize(2);
      label = (ghDepth == 0) ? "<< MENU" : "< BACK";
    } else if (idx == 1 && ghDepth > 0) {
      lcd.setTextSize(2);
      label = "<< MENU";
    } else {
      lcd.setTextSize(3);
      int eIdx = idx - headerRows;
      label = String(ghEntries[eIdx].name);
      if (label.length() > 22) label = label.substring(0, 21) + "~";
      if (!ghEntries[eIdx].isDir) label = "  " + label;
    }

    int bw = LIST_BTN_W, bh = LIST_BTN_H;
    drawBtn(8, y, bw, bh, TFT_DARKGREY, label.c_str());
  }
  drawScrollbar(ghScroll, totalRows, LIST_ROW_Y0, LIST_MAX_VIS * LIST_ROW_H);
  drawFooter(); lcd.display();
}

// Enter the GitHub browser at a given repo path.
// push=true saves current depth to stack (for BACK navigation).
void enterGhBrowse(const String& repoPath, bool push) {
  DBGF("[GH]  enterGhBrowse path='%s' push=%d\n", repoPath.c_str(), push);
  appState = S_GH_BROWSE;

  if (push && ghDepth < GH_MAX_DEPTH) {
    ghStack[ghDepth++] = repoPath;
  }

  ghSel = 0;
  ghScroll = 0;

  if (WiFi.status() != WL_CONNECTED) {
    ledSet(255, 80, 0);  // orange = no WiFi
    showStatus2("GitHub Browser", "No WiFi!");
    delay(2000);
    enterMainMenu();
    return;
  }

  ledScanPulse();  // blue while loading
  showStatus2("Github Library", "Loading");

  bool ok = ghFetchDir(repoPath);
  if (!ok) {
    ledFlash(255, 0, 0, 3);
    showStatus2("GitHub Library", "Fetch failed!");
    delay(2000);
    enterMainMenu();
    return;
  }
  ledSet(0, 0, 40);  // dim blue = browsing
  drawGhBrowser();
}

void enterReadTag() {
  DBGLN("[STATE] -> READ_TAG");
  appState = S_READ_TAG;
  showStatus("Read Tag\nPlace Bambu tag\non reader");
}

void enterCloneSource() {
  DBGLN("[STATE] -> CLONE_SOURCE");
  appState = S_CLONE_SOURCE;
  showStatus("Clone Tag\nCLONE  Step 1/2\nPlace SOURCE tag\non reader");
}

void enterFatBrowser() {
  DBGLN("[STATE] -> FAT_BROWSE (S_DUMP_SELECT)");
  appState = S_DUMP_SELECT;
  fatDepth = 0;
  fatCurPath = "/";
  fatLoadDir("/");
  drawFatBrowser();
}

static void countDumpFiles(const String& path, int& count) {
  File dir = FFat.open(path);
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f) {
    if (f.isDirectory()) {
      String sub = path == "/" ? String("/") + f.name() : path + "/" + f.name();
      countDumpFiles(sub, count);
    } else {
      String n = f.name();
      if (n.endsWith(".bin")) count++;
    }
    f = dir.openNextFile();
  }
}

void enterBmCatBrowse(int level);
void enterBmBrowse() { enterBmCatBrowse(0); }
void enterBmCatBrowse(int level) {
  bmCatLevel = level;
  bmCatSel = 0;
  bmCatScroll = 0;
  appState = S_BM_CAT_BROWSE;

  if (level > 0 && WiFi.status() != WL_CONNECTED) {
    showStatus2("BambuMan", "No WiFi!");
    delay(1500);
    appState = S_WIFI_INFO;
    return;
  }

  bmCatCount = 0;
  if (FFat.exists("/BM/catalog.json")) {
    showStatus2("BambuMan Library", "Loading");
    ledScanPulse();
    if (!bmCatLoadLevel() && level > 0) {
      showStatus("BambuMan Library\nCatalog read\nfailed.");
      appState = S_WIFI_INFO;
      return;
    }
  }

  ledSet(0, 0, 40);
  drawBmCatBrowser();
}

void enterWifiInfo() {
  DBGLN("[STATE] -> SYSTEM");
  appState = S_WIFI_INFO;
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("System");
  lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(2);

  int y = 120;
  int c1 = 10, c2 = 190;

  if (WiFi.status() == WL_CONNECTED) {
    ledSet(0, 80, 80);
    lcd.setCursor(c1, y); lcd.print("WiFi:");  lcd.setCursor(c2, y); lcd.print("Connected"); y += 26;
    lcd.setCursor(c1, y); lcd.print("SSID:");  lcd.setCursor(c2, y); lcd.print(wifiSSID); y += 26;
    lcd.setCursor(c1, y); lcd.print("IP:");    lcd.setCursor(c2, y); lcd.print(WiFi.localIP().toString()); y += 26;
    lcd.setCursor(c1, y); lcd.print("Mode:");  lcd.setCursor(c2, y); lcd.print("Station (STA)"); y += 26;
  } else {
    ledSet(80, 40, 0);
    lcd.setCursor(c1, y); lcd.print("WiFi:");  lcd.setCursor(c2, y); lcd.print("Not connected"); y += 26;
    lcd.setCursor(c1, y); lcd.print("Mode:");  lcd.setCursor(c2, y); lcd.print("Access Point (AP)"); y += 26;
    lcd.setCursor(c1, y); lcd.print("SSID:");  lcd.setCursor(c2, y); lcd.print(AP_SSID); y += 26;
    lcd.setCursor(c1, y); lcd.print("IP:");    lcd.setCursor(c2, y); lcd.print("192.168.4.1"); y += 26;
  }
  y += 8;

  int heap = ESP.getFreeHeap();
  int fatTotal = FFat.totalBytes();
  int fatUsed = FFat.usedBytes();

  lcd.setCursor(c1, y); lcd.print("Free Heap:"); lcd.setCursor(c2, y); lcd.print(heap); lcd.print(" bytes"); y += 26;
  lcd.setCursor(c1, y); lcd.print("FAT:");       lcd.setCursor(c2, y); lcd.print(fatUsed / 1024); lcd.print(" / "); lcd.print(fatTotal / 1024); lcd.print(" kB"); y += 26;

  // Count dump files
  int dumpCount = 0;
  countDumpFiles("/", dumpCount);
  lcd.setCursor(c1, y); lcd.print("Tag Dumps:"); lcd.setCursor(c2, y); lcd.print(dumpCount); y += 26;

  // Delete all tags button
  int btnY = 360;
  drawBtn(200, btnY, 400, 56, TFT_MAROON, "Delete All Tags");

  drawFooter(); lcd.display();
}

// ──────────────────────────────────────────────────────────────
//  Main-menu encoder handler  (non-blocking)
// ──────────────────────────────────────────────────────────────
// ──────────────────────────────────────────────────────────────
//  BambuMan catalog OLED browser  (4-level: Mat→Type→Color→UID)
// ──────────────────────────────────────────────────────────────

// ── BambuMan cache helpers ─────────────────────────────────────────────────

// Single-pass stream build of all 3 navigation levels from /BM/catalog.json.
// Called automatically on first OLED browse; returns false if file missing.
bool bmCacheBuild() {
  if (bmCacheValid) return true;
  bmCL0n = 0; bmCL1n = 0; bmCL2n = 0;

  File f = FFat.open("/BM/catalog.json", "r");
  if (!f) return false;

  while (f.available()) if (f.read() == '[') break;

  StaticJsonDocument<384> doc;
  char obj[256];

  while (f.available()) {
    char c;
    do {
      if (!f.available()) goto bmBuildDone;
      c = f.read();
    } while (c != '{');

    obj[0] = '{';
    int i = 1, depth = 1;
    while (f.available() && i < 190) {
      c = f.read();
      obj[i++] = c;
      if (c == '{') depth++;
      else if (c == '}') { if (--depth == 0) break; }
    }
    obj[i] = '\0';

    doc.clear();
    if (deserializeJson(doc, obj)) continue;

    const char* m  = doc["m"] | "";
    const char* t  = doc["t"] | "";
    const char* co = doc["c"] | "";
    if (!m[0]) continue;

    // L0 – unique materials
    {
      bool found = false;
      for (int j = 0; j < bmCL0n; j++)
        if (strcmp(bmCL0[j].mat, m) == 0) { found = true; break; }
      if (!found && bmCL0n < BM_CACHE_L0) {
        strncpy(bmCL0[bmCL0n].mat, m, 31); bmCL0[bmCL0n].mat[31] = '\0';
        bmCL0n++;
      }
    }
    // L1 – unique mat+type combos
    {
      bool found = false;
      for (int j = 0; j < bmCL1n; j++)
        if (strcmp(bmCL1[j].mat, m) == 0 && strcmp(bmCL1[j].type, t) == 0) { found = true; break; }
      if (!found && bmCL1n < BM_CACHE_L1) {
        strncpy(bmCL1[bmCL1n].mat,  m, 31); bmCL1[bmCL1n].mat[31]  = '\0';
        strncpy(bmCL1[bmCL1n].type, t, 31); bmCL1[bmCL1n].type[31] = '\0';
        bmCL1n++;
      }
    }
    // L2 – unique mat+type+color combos
    {
      bool found = false;
      for (int j = 0; j < bmCL2n; j++)
        if (strcmp(bmCL2[j].mat, m) == 0 && strcmp(bmCL2[j].type, t) == 0
            && strcmp(bmCL2[j].color, co) == 0) { found = true; break; }
      if (!found && bmCL2n < BM_CACHE_L2) {
        strncpy(bmCL2[bmCL2n].mat,   m,  31); bmCL2[bmCL2n].mat[31]   = '\0';
        strncpy(bmCL2[bmCL2n].type,  t,  31); bmCL2[bmCL2n].type[31]  = '\0';
        strncpy(bmCL2[bmCL2n].color, co, 31); bmCL2[bmCL2n].color[31] = '\0';
        bmCL2n++;
      }
    }
    yield();
  }

bmBuildDone:
  f.close();
  bmCacheValid = true;
  DBGF("[BM] Cache built: L0=%d  L1=%d  L2=%d\n", bmCL0n, bmCL1n, bmCL2n);
  return true;
}

// Populate bmCatEntries[] for the current browse level.
// Levels 0–2: served from RAM cache (fast, no SD read).
// Level 3 (UIDs): stream-parsed from catalog.json (list is unbounded).
// Returns false only if /BM/catalog.json is missing entirely.
bool bmCatLoadLevel() {
  bmCatCount = 0;

  // ── Levels 0–2: cache path ────────────────────────────────
  if (bmCatLevel < 3) {
    if (!bmCacheBuild()) return false;   // catalog missing

    if (bmCatLevel == 0) {
      for (int i = 0; i < bmCL0n && bmCatCount < BM_MAX_ENTRIES; i++) {
        strncpy(bmCatEntries[bmCatCount].label, bmCL0[i].mat, 31);
        bmCatEntries[bmCatCount++].label[31] = '\0';
      }
    } else if (bmCatLevel == 1) {
      for (int i = 0; i < bmCL1n && bmCatCount < BM_MAX_ENTRIES; i++) {
        if (strcmp(bmCL1[i].mat, bmCatMat) != 0) continue;
        strncpy(bmCatEntries[bmCatCount].label, bmCL1[i].type, 31);
        bmCatEntries[bmCatCount++].label[31] = '\0';
      }
    } else {   // level 2
      for (int i = 0; i < bmCL2n && bmCatCount < BM_MAX_ENTRIES; i++) {
        if (strcmp(bmCL2[i].mat,  bmCatMat)  != 0) continue;
        if (strcmp(bmCL2[i].type, bmCatType) != 0) continue;
        strncpy(bmCatEntries[bmCatCount].label, bmCL2[i].color, 31);
        bmCatEntries[bmCatCount++].label[31] = '\0';
      }
    }
    return true;
  }

  // ── Level 3 (UIDs): stream-parse catalog.json ─────────────
  File f = FFat.open("/BM/catalog.json", "r");
  if (!f) return false;

  while (f.available()) if (f.read() == '[') break;

  StaticJsonDocument<384> doc;
  char obj[256];

  while (f.available() && bmCatCount < BM_MAX_ENTRIES) {
    char c;
    do {
      if (!f.available()) goto bmLoadDone;
      c = f.read();
    } while (c != '{');

    obj[0] = '{';
    int i = 1, depth = 1;
    while (f.available() && i < 190) {
      c = f.read();
      obj[i++] = c;
      if (c == '{') depth++;
      else if (c == '}') { if (--depth == 0) break; }
    }
    obj[i] = '\0';

    doc.clear();
    if (deserializeJson(doc, obj)) continue;

    const char* m  = doc["m"] | "";
    const char* t  = doc["t"] | "";
    const char* co = doc["c"] | "";
    const char* u  = doc["u"] | "";

    if (strcmp(m,  bmCatMat)   != 0) continue;
    if (strcmp(t,  bmCatType)  != 0) continue;
    if (strcmp(co, bmCatColor) != 0) continue;
    if (!u[0]) continue;

    strncpy(bmCatEntries[bmCatCount].label, u, 31);
    bmCatEntries[bmCatCount++].label[31] = '\0';
  }
bmLoadDone:
  f.close();
  return true;
}

void drawBmCatBrowser() {
  lcd.fillScreen(TFT_BLACK);

  drawStatusBar(); drawSubHeader("BambuMan Library");

  // Breadcrumb
  if (bmCatLevel > 0) {
    lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK); lcd.setTextSize(2);
    lcd.setCursor(12, 120);
    String crumb = bmCatMat;
    if (bmCatLevel >= 2) { crumb += " > "; crumb += bmCatType; }
    if (bmCatLevel >= 3) { crumb += " > "; crumb += bmCatColor; }
    if (crumb.length() > 42) crumb = "..." + crumb.substring(crumb.length() - 39);
    lcd.print(crumb);
  }

  // Side-by-side sync buttons at level 0
  int listY0 = LIST_ROW_Y0;
  if (bmCatLevel == 0) {
    int btnY = 140;
    drawBtn(8,  btnY, 384, 46, TFT_DARKGREY, "Sync Catalog");
    drawBtn(400, btnY, 384, 46, TFT_DARKGREY, "Full Download");
    listY0 = btnY + 56;
  }

  int syncExtra = 0;
  int backExtra = (bmCatLevel > 0) ? 1 : 0;
  int totalRows = bmCatCount + syncExtra + backExtra;

  int visRows = LIST_MAX_VIS;
  if (bmCatLevel == 0) visRows = min(LIST_MAX_VIS, (FOOTER_Y - listY0) / LIST_ROW_H);

  if (bmCatLevel == 0 && bmCatCount == 0) {
    lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(3);
    lcd.setCursor(10, listY0 + 10); lcd.print("No catalog");
    lcd.setCursor(10, listY0 + 38); lcd.print("Sync or full download");
  }

  for (int row = 0; row < visRows; row++) {
    int idx = bmCatScroll + row;
    if (idx >= totalRows) break;

    int y = listY0 + row * LIST_ROW_H;
    bool sel = (idx == bmCatSel);

    String label;
    if (idx == 0 && bmCatLevel > 0) {
      lcd.setTextSize(2);
      label = "< BACK";
    } else {
      lcd.setTextSize(3);
      int eIdx = idx - backExtra;
      if (eIdx < 0 || eIdx >= bmCatCount) break;
      label = String(bmCatEntries[eIdx].label);
      if (label.length() > 22) label = label.substring(0, 21) + "~";
    }

    int bw = LIST_BTN_W, bh = LIST_BTN_H;
    drawBtn(8, y, bw, bh, TFT_DARKGREY, label.c_str());
  }
  int scrollH = visRows * LIST_ROW_H;
  drawScrollbar(bmCatScroll, totalRows, listY0, scrollH);
  drawFooter(); lcd.display();
}

// (Re-)enter the catalog browser at the given level; loads entries from FAT.
// ── OLED-driven BambuMan catalog sync ─────────────────────────────────────
// Quick sync: fetch central directory only via Range request, build catalog.json
void bmOledSyncCatalogQuick() {
  if (WiFi.status() != WL_CONNECTED) {
    showStatus2("BambuMan Sync", "No WiFi!");
    delay(2000);
    enterBmCatBrowse(0);
    return;
  }

  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(3);
  lcd.setCursor(10, 138); lcd.print("1/3 Find ZIP...");
  drawFooter(); lcd.display();
  ledSet(0, 0, 80);

  String zipUrl = bmFindZipUrl();
  if (zipUrl.isEmpty()) {
    showStatus2("BambuMan Sync", "ZIP not found");
    delay(3000);
    enterBmCatBrowse(0);
    return;
  }

  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setCursor(10, 138); lcd.print("2/3 Read catalog...");
  drawFooter(); lcd.display();

  // HEAD → file size
  long fileSize = 0;
  {
    HTTPClient hc;
    hc.begin(zipUrl);
    hc.addHeader("User-Agent", "Mozilla/5.0 (compatible; BambuTagger/1.0; ESP32)");
    hc.sendRequest("HEAD");
    fileSize = hc.getSize();
    hc.end();
  }
  if (fileSize <= 0) {
    showStatus2("BambuMan Sync", "HEAD failed");
    delay(3000);
    enterBmCatBrowse(0);
    return;
  }

  // Fetch last 512 B, find EOCD
  uint8_t tail[512] = {};
  {
    long ts = fileSize - 512;
    HTTPClient hc;
    hc.begin(zipUrl);
    hc.addHeader("User-Agent", "Mozilla/5.0 (compatible; BambuTagger/1.0; ESP32)");
    hc.addHeader("Range", "bytes=" + String(ts) + "-");
    hc.GET();
    WiFiClient* s = hc.getStreamPtr();
    int got = 0;
    unsigned long t0 = millis();
    while (got < 512 && millis() - t0 < 12000) {
      int r = s->readBytes(tail + got, 512 - got);
      if (r > 0) { got += r; t0 = millis(); }
      else delay(10);
    }
    hc.end();
    if (got < 22) {
      showStatus2("BambuMan Sync", "Short tail");
      delay(3000);
      enterBmCatBrowse(0);
      return;
    }
  }

  long cd_offset = -1, cd_size = -1;
  for (int i = 510; i >= 0; i--) {
    if (tail[i] == 0x50 && tail[i+1] == 0x4B && tail[i+2] == 0x05 && tail[i+3] == 0x06) {
      cd_size = (long)tail[i+12] | ((long)tail[i+13]<<8) | ((long)tail[i+14]<<16) | ((long)tail[i+15]<<24);
      cd_offset = (long)tail[i+16] | ((long)tail[i+17]<<8) | ((long)tail[i+18]<<16) | ((long)tail[i+19]<<24);
      break;
    }
  }
  if (cd_offset < 0 || cd_size <= 0) {
    showStatus2("BambuMan Sync", "EOCD not found");
    delay(3000);
    enterBmCatBrowse(0);
    return;
  }

  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setCursor(10, 138); lcd.print("3/3 Writing...");
  drawFooter(); lcd.display();
  ledSet(255, 200, 0);

  if (!FFat.exists("/BM")) FFat.mkdir("/BM");
  FFat.remove("/BM/catalog.json");
  File outF = FFat.open("/BM/catalog.json", "w");
  if (!outF) {
    showStatus2("BambuMan Sync", "FAT write fail");
    delay(3000);
    enterBmCatBrowse(0);
    return;
  }

  int count = 0;
  {
    WiFiClientSecure wcs; wcs.setInsecure();
    HTTPClient hc;
    hc.begin(wcs, zipUrl);
    hc.addHeader("User-Agent", "Mozilla/5.0 (compatible; BambuTagger/1.0; ESP32)");
    hc.addHeader("Range", "bytes=" + String(cd_offset) + "-" + String(cd_offset + cd_size - 1));
    hc.setTimeout(90000);
    int code = hc.GET();
    if (code != 200 && code != 206) {
      outF.close(); hc.end();
      showStatus2("BambuMan Sync", ("CD err " + String(code)).c_str());
      delay(3000);
      enterBmCatBrowse(0);
      return;
    }
    WiFiClient* stream = hc.getStreamPtr();
    uint8_t hdr[46], fname[280];
    bool first = true;
    long remaining = cd_size;
    outF.print("[");

    while (remaining >= 46) {
      if (!bmReadExact(stream, hdr, 46)) break;
      remaining -= 46;
      if (hdr[0] != 0x50 || hdr[1] != 0x4B || hdr[2] != 0x01 || hdr[3] != 0x02) break;
      uint16_t fnLen = (uint16_t)hdr[28] | ((uint16_t)hdr[29] << 8);
      uint16_t exLen = (uint16_t)hdr[30] | ((uint16_t)hdr[31] << 8);
      uint16_t cmLen = (uint16_t)hdr[32] | ((uint16_t)hdr[33] << 8);

      int fnRead = min((int)fnLen, 279);
      if (!bmReadExact(stream, fname, fnRead)) break;
      fname[fnRead] = 0;
      remaining -= fnLen;
      if (fnLen > fnRead) { bmSkipBytes(stream, fnLen - fnRead); remaining -= (fnLen - fnRead); }
      if (exLen > 0) { bmSkipBytes(stream, exLen); remaining -= exLen; }
      if (cmLen > 0) { bmSkipBytes(stream, cmLen); remaining -= cmLen; }

      String path = String((char*)fname);
      if (!path.endsWith("/data.bin")) continue;

      int s0 = path.indexOf('/');
      int s1 = s0 >= 0 ? path.indexOf('/', s0+1) : -1;
      int s2 = s1 >= 0 ? path.indexOf('/', s1+1) : -1;
      int s3 = s2 >= 0 ? path.indexOf('/', s2+1) : -1;
      if (s0 < 0 || s1 < 0 || s2 < 0 || s3 < 0) continue;
      String mat = path.substring(0, s0);
      String typ = path.substring(s0+1, s1);
      String col = path.substring(s1+1, s2);
      String uid = path.substring(s2+1, s3);
      if (uid.length() < 4 || uid.length() > 12) continue;
      mat.replace("\"", "\\"); typ.replace("\"", "\\");
      col.replace("\"", "\\"); uid.replace("\"", "\\");

      if (!first) outF.print(","); first = false;
      outF.print("{\"u\":\""); outF.print(uid);
      outF.print("\",\"m\":\""); outF.print(mat);
      outF.print("\",\"t\":\""); outF.print(typ);
      outF.print("\",\"c\":\""); outF.print(col);
      outF.print("\"}"); count++;
      yield();
    }
    outF.print("]"); outF.close();
    hc.end();
  }

  bmCacheInvalidate();
  ledFlash(0, 255, 0, 3);
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setCursor(10, 138); lcd.print("Done!");
  lcd.setCursor(10, 166); lcd.print(String(count) + " entries");
  drawFooter(); lcd.display();
  ledSet(0, 0, 40);

  unsigned long t0 = millis();
  while (millis() - t0 < 10000) {
    httpServer.handleClient();
    if (touchPoll()) break;
    delay(20);
  }
  enterBmCatBrowse(0);
}

// Full download: fetches entire ZIP and extracts data.bin files
// Downloads the full daily ZIP and extracts data.bin files into the FAT
// directory structure. Builds /BM/catalog.json for the 4-level browser.
void bmOledSyncCatalog() {
  if (WiFi.status() != WL_CONNECTED) {
    showStatus2("BambuMan Sync", "No WiFi!");
    delay(2000);
    enterBmCatBrowse(0);
    return;
  }

  // Step 1 – find ZIP URL
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setCursor(10, 138);
  lcd.print("1/4 Find ZIP...");
  drawFooter();
  ledSet(0, 0, 80);

  String zipUrl = bmFindZipUrl();
  if (zipUrl.isEmpty()) {
    showStatus2("BambuMan Sync", "ZIP not found");
    delay(3000);
    enterBmCatBrowse(0);
    return;
  }

  // Step 2 – HEAD → file size
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setCursor(10, 138);
  lcd.print("2/4 Getting size...");
  drawFooter();
  long fileSize = 0;
  {
    HTTPClient hc;
    hc.begin(zipUrl);
    hc.addHeader("User-Agent", "Mozilla/5.0 (compatible; BambuTagger/1.0; ESP32)");
    hc.sendRequest("HEAD");
    fileSize = hc.getSize();
    hc.end();
  }
  if (fileSize <= 0) {
    showStatus2("BambuMan Sync", "HEAD failed");
    delay(3000);
    enterBmCatBrowse(0);
    return;
  }

  // Step 3 – download full ZIP and extract
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setCursor(10, 138);
  lcd.print("3/4 Download & extract...");
  drawFooter();
  ledSet(255, 200, 0);

  // Ensure fresh catalog
  if (!FFat.exists("/BM")) FFat.mkdir("/BM");
  FFat.remove("/BM/catalog.json");

  // Open catalog.json for writing
  File catF = FFat.open("/BM/catalog.json", "w");
  if (!catF) {
    showStatus2("BambuMan Sync", "FAT write fail");
    delay(3000);
    enterBmCatBrowse(0);
    return;
  }
  catF.print("[");

  int count = 0;
  bool firstCat = true;
  long downloaded = 0;
  unsigned long lastDraw = 0;

  WiFiClientSecure wcs;
  wcs.setInsecure();
  HTTPClient hc;
  hc.begin(wcs, zipUrl);
  hc.addHeader("User-Agent", "Mozilla/5.0 (compatible; BambuTagger/1.0; ESP32)");
  int code = hc.GET();
  if (code != 200) {
    catF.print("]"); catF.close();
    hc.end();
    char err[20];
    snprintf(err, sizeof(err), "HTTP %d", code);
    showStatus2("BambuMan Sync", err);
    delay(3000);
    enterBmCatBrowse(0);
    return;
  }

  WiFiClient* stream = hc.getStreamPtr();
  uint8_t lhdr[30];
  uint8_t fname[280];

  while (true) {
    // Read local file header signature (4 bytes)
    uint32_t sig = 0;
    if (!bmReadExact(stream, (uint8_t*)&sig, 4)) break;
    if (sig != 0x04034b50) {
      // Central directory or end of archive – stop processing local entries
      break;
    }
    downloaded += 4;

    // Read rest of local header (26 bytes after signature)
    if (!bmReadExact(stream, lhdr, 26)) break;
    downloaded += 26;

    uint16_t compMethod = lhdr[4] | ((uint16_t)lhdr[5] << 8);
    uint32_t compSize   = (uint32_t)lhdr[14] | ((uint32_t)lhdr[15] << 8)
                        | ((uint32_t)lhdr[16] << 16) | ((uint32_t)lhdr[17] << 24);
    uint32_t uncompSize = (uint32_t)lhdr[18] | ((uint32_t)lhdr[19] << 8)
                        | ((uint32_t)lhdr[20] << 16) | ((uint32_t)lhdr[21] << 24);
    uint16_t fnLen = lhdr[22] | ((uint16_t)lhdr[23] << 8);
    uint16_t exLen = lhdr[24] | ((uint16_t)lhdr[25] << 8);

    // Read filename
    int fnRead = min((int)fnLen, 279);
    if (!bmReadExact(stream, fname, fnRead)) break;
    fname[fnRead] = 0;
    downloaded += fnLen;
    if (fnLen > fnRead) {
      bmSkipBytes(stream, fnLen - fnRead);
      downloaded += (fnLen - fnRead);
    }

    // Skip extra field
    if (exLen > 0) {
      bmSkipBytes(stream, exLen);
      downloaded += exLen;
    }

    String path = String((char*)fname);

    // Progress update
    if (millis() - lastDraw > 500) {
      lastDraw = millis();
      int pct = fileSize > 0 ? (int)(downloaded * 100LL / fileSize) : 0;
      lcd.fillScreen(TFT_BLACK);
      drawStatusBar(); drawSubHeader("BambuMan Sync");
      lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(3);
      lcd.setCursor(10, 138);
      lcd.print("3/4 Extracting...");
      lcd.setCursor(10, 166);
      lcd.print(String(pct) + "%  " + String(count) + " files");
      drawFooter();
      yield();
    }

    // Process data.bin files only
    if (!path.endsWith("/data.bin")) {
      // Skip file data
      if (compMethod == 0) {
        bmSkipBytes(stream, compSize);
        downloaded += compSize;
      } else {
        bmSkipBytes(stream, compSize);
        downloaded += compSize;
      }
      continue;
    }

    // Parse path components: Mat/Type/Color/UID/data.bin
    int s0 = path.indexOf('/');
    int s1 = s0 >= 0 ? path.indexOf('/', s0 + 1) : -1;
    int s2 = s1 >= 0 ? path.indexOf('/', s1 + 1) : -1;
    int s3 = s2 >= 0 ? path.indexOf('/', s2 + 1) : -1;
    if (s0 < 0 || s1 < 0 || s2 < 0 || s3 < 0) {
      bmSkipBytes(stream, compSize);
      downloaded += compSize;
      continue;
    }
    String mat = path.substring(0, s0);
    String typ = path.substring(s0 + 1, s1);
    String col = path.substring(s1 + 1, s2);
    String uid = path.substring(s2 + 1, s3);

    // Sanitize for JSON
    mat.replace("\"", "\\"); typ.replace("\"", "\\");
    col.replace("\"", "\\"); uid.replace("\"", "\\");

    // Write to catalog.json
    if (!firstCat) catF.print(",");
    firstCat = false;
    catF.print("{\"u\":\"");
    catF.print(uid);
    catF.print("\",\"m\":\"");
    catF.print(mat);
    catF.print("\",\"t\":\"");
    catF.print(typ);
    catF.print("\",\"c\":\"");
    catF.print(col);
    catF.print("\"}");

    // Build directory: /Mat/Type/Color/
    String dir = "/" + mat;
    if (!FFat.exists(dir)) FFat.mkdir(dir);
    dir += "/" + typ;
    if (!FFat.exists(dir)) FFat.mkdir(dir);
    dir += "/" + col;
    if (!FFat.exists(dir)) FFat.mkdir(dir);

    // Save data.bin as UID.bin
    String filePath = dir + "/" + uid + ".bin";
    File outF = FFat.open(filePath, "w");
    if (outF) {
      bool ok = false;
      if (compMethod == 0 && compSize == DUMP_SIZE) {
        uint8_t buf[256];
        long remain = compSize;
        ok = true;
        while (remain > 0) {
          int chunk = min((long)sizeof(buf), remain);
          if (!bmReadExact(stream, buf, chunk)) { ok = false; break; }
          outF.write(buf, chunk);
          remain -= chunk;
          downloaded += chunk;
        }
      } else if (compMethod == 8) {
        ok = bmInflateToFile(stream, compSize, uncompSize, outF);
        downloaded += compSize;
      } else {
        bmSkipBytes(stream, compSize);
        downloaded += compSize;
      }
      outF.close();
      if (ok) count++;
      else FFat.remove(filePath);
    } else {
      bmSkipBytes(stream, compSize);
      downloaded += compSize;
    }
  }

  catF.print("]");
  catF.flush();
  catF.close();
  hc.end();

  DBGF("[BM] sync done: %d files\n", count);
  bmCacheInvalidate();

  // Success screen
  ledFlash(0, 255, 0, 3);
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setCursor(10, 138);
  lcd.print("Done!");
  lcd.setCursor(10, 166);
  lcd.print(String(count) + " files");
  drawFooter();
  ledSet(0, 0, 40);

  unsigned long t0 = millis();
  while (millis() - t0 < 10000) {
    httpServer.handleClient();
    if (touchPoll()) break;
    delay(20);
  }
  enterBmCatBrowse(0);
}

// Fetch a dump from bambuman.ee by UID. Returns saved path or "" on error.
// Caller must show confirmation / error feedback.
String bmCatFetchUid(const String& uid) {
  showStatus2("BambuMan", ("Fetching " + uid).c_str());
  DBGF("[BM]  Catalog fetch uid=%s\n", uid.c_str());


  String url = "https://bambuman.ee/dl/tags/" + uid + "/data.bin";
  WiFiClientSecure wcs;
  wcs.setInsecure();
  HTTPClient http;
  http.begin(wcs, url);
  http.addHeader("User-Agent", "Mozilla/5.0 (compatible; BambuTagger/1.0; ESP32)");
  http.addHeader("Accept", "application/octet-stream");
  int code = http.GET();

  if (code != 200) {
    http.end();
    DBGF("[BM]  HTTP %d\n", code);
    String msg = "BambuMan\nHTTP " + String(code);
    if (code == 404) msg = "BambuMan\nUID not found";
    if (code == 403) msg = "BambuMan\nBlocked (CF)\nTry Web UI";
    showStatus(("BambuMan Library\n" + msg).c_str());
    ledFlash(255, 0, 0, 2);
    return "";
  }

  int totalSize = http.getSize();
  if (totalSize > 0 && totalSize != DUMP_SIZE) {
    http.end();
    showStatus(("BambuMan Library\nBad size:\n" + String(totalSize)).c_str());
    ledFlash(255, 0, 0, 2);
    return "";
  }

  // ── Resolve save path: catalog context → catalog lookup → fallback ────────
  String mat(bmCatMat), typ(bmCatType), col(bmCatColor);
  // If m/t/c globals not set (e.g. scan-tag flow), try catalog.json lookup
  if (mat.isEmpty() || typ.isEmpty() || col.isEmpty()) {
    String lm, lt, lc;
    if (bmLookupCatalog(uid, lm, lt, lc)) {
      if (mat.isEmpty()) mat = lm;
      if (typ.isEmpty()) typ = lt;
      if (col.isEmpty()) col = lc;
      DBGF("[BM]  Catalog lookup hit: %s/%s/%s\n", mat.c_str(), typ.c_str(), col.c_str());
    }
  }
  String savePath;
  if (!mat.isEmpty() && !typ.isEmpty() && !col.isEmpty()) {
    savePath = buildBmFilePath(mat, typ, col, uid);
  } else {
    if (!FFat.exists("/BM")) FFat.mkdir("/BM");
    savePath = "/BM/" + uid + ".bin";
    DBGLN("[BM]  No m/t/c — using fallback path");
  }
  ensureParentDirs(savePath);
  File f = FFat.open(savePath, "w");
  if (!f) {
    http.end();
    showStatus("BambuMan Library\nFFat write\nfailed!");
    ledFlash(255, 0, 0, 2);
    return "";
  }

  int written = http.writeToStream(&f);
  f.close();
  http.end();

  if (written != DUMP_SIZE) {
    FFat.remove(savePath);
    DBGF("[BM]  incomplete %d/%d\n", written, DUMP_SIZE);
    showStatus(("BambuMan Library\nIncomplete:\n" + String(written) + "/" + String(DUMP_SIZE) + "").c_str());
    ledFlash(255, 0, 0, 2);
    return "";
  }

  bmIndexAdd(savePath);
  DBGF("[BM]  Saved %s\n", savePath.c_str());
  return savePath;
}

// ── Keep legacy scan-by-tag flow for programmatic use ─────────────────────────
void processBmBrowse() {
  unsigned long deadline = millis() + 20000;
  while (millis() < deadline) {
    httpServer.handleClient();
    ledScanPulse();
    if (touchPoll()) { enterMainMenu(); return; }
    if (!rfid.PICC_IsNewCardPresent()) { delay(18); continue; }
    if (!rfid.PICC_ReadCardSerial()) { delay(18); continue; }
    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      if (rfid.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    String saved = bmCatFetchUid(uid);
    if (saved.isEmpty()) { appState = S_WIFI_INFO; return; }
    ledFlash(0, 255, 0, 2);
    showStatus(("BambuMan Library\nOK!\n\n" + saved).c_str());
    appState = S_WIFI_INFO;
    return;
  }
  showStatus("BambuMan Library\nNo tag detected.");
  ledFlash(255, 0, 0, 2);
  appState = S_WIFI_INFO;
}

void processReadTag() {
  DBGLN("[RFID] processReadTag: waiting for tag (15 s)...");
  unsigned long deadline = millis() + 15000;
  while (millis() < deadline) {
    httpServer.handleClient();
    if (touchPoll()) {
      enterMainMenu();
      return;
    }
    if (rfidReadBambuTag(&currentTag)) {
      DBGF("[RFID] Tag read OK: %s / %s  color=#%02X%02X%02X\n",
           currentTag.filamentType, currentTag.detailedType,
           currentTag.colorR, currentTag.colorG, currentTag.colorB);
      ledSetTagColor(&currentTag);  // show filament colour
      appState = S_SHOW_TAG;
      drawTagInfo(&currentTag, 0);
      return;
    }
    delay(18);
  }
  DBGLN("[RFID] processReadTag: timeout – no tag.");
  ledFlash(255, 0, 0, 2);
  showStatus("Read Tag\nNo tag detected.");
  appState = S_WIFI_INFO;
}

void processCloneSource() {
  DBGLN("[CLONE] processCloneSource: waiting for source tag (15 s)...");
  unsigned long deadline = millis() + 15000;
  while (millis() < deadline) {
    httpServer.handleClient();
    if (touchPoll()) {
      enterMainMenu();
      return;
    }
    if (rfidReadBambuTag(&sourceTag)) {
      DBGF("[CLONE] Source tag read: %s / %s  UID=%02X%02X%02X%02X\n",
           sourceTag.filamentType, sourceTag.detailedType,
           sourceTag.uid[0], sourceTag.uid[1],
           sourceTag.uid[2], sourceTag.uid[3]);
      ledSetTagColor(&sourceTag);  // flash source colour briefly
      tagToFlat(&sourceTag, dumpBuf);
      showStatus2("Source read OK!", "Place TARGET card\x85");
      delay(1500);
      ledSet(255, 165, 0);  // orange = waiting for target card
      showStatus("Clone Tag\nCLONE  Step 2/2\nPlace TARGET card\non reader\x85");
      appState = S_CLONE_TARGET;
      return;
    }
    delay(18);
  }
  DBGLN("[CLONE] processCloneSource: timeout – no tag.");
  ledFlash(255, 0, 0, 2);
  showStatus("Clone Tag\nTimeout. No tag.");
  appState = S_WIFI_INFO;
}

void processCloneTarget() {
  DBGLN("[CLONE] processCloneTarget: waiting for target card (15 s)...");
  unsigned long deadline = millis() + 15000;
  while (millis() < deadline) {
    httpServer.handleClient();
    if (touchPoll()) {
      enterMainMenu();
      return;
    }
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      DBGF("[CLONE] Target card UID: %02X %02X %02X %02X – starting write...\n",
           rfid.uid.uidByte[0], rfid.uid.uidByte[1],
           rfid.uid.uidByte[2], rfid.uid.uidByte[3]);
      ledSet(255, 255, 0);  // yellow = writing in progress
      showStatus("Clone Tag\nWriting");

      int sectOk = rfidWriteDump(dumpBuf, true);
      DBGF("[CLONE] Write result: %d/%d sectors OK\n", sectOk, NUM_SECTORS);
      bool ok = (sectOk == NUM_SECTORS);
      bool partial = (sectOk > 0 && sectOk < NUM_SECTORS);
      if (ok) {
        ledFlash(0, 255, 0, 3);  // 3× green = success
      } else if (partial) {
        ledFlash(255, 165, 0, 3); // 3× amber = partial
      } else {
        ledFlash(255, 0, 0, 3);  // 3× red = fail
      }
      char cloneMsg[64];
      if (ok)
        snprintf(cloneMsg, sizeof(cloneMsg), "Clone complete!");
      else if (partial)
        snprintf(cloneMsg, sizeof(cloneMsg), "Partial! %d/16 sec\nCard already keyed?", sectOk);
      else
        snprintf(cloneMsg, sizeof(cloneMsg), "Write failed!\nTry a magic/FUID\ncard.");
      showStatus((String("Clone Tag\n") + cloneMsg).c_str());
      appState = S_WIFI_INFO;
      return;
    }
    delay(18);
  }
  DBGLN("[CLONE] processCloneTarget: timeout – no card.");
  ledFlash(255, 0, 0, 2);
  showStatus("Clone Tag\nTimeout. No card.");
  appState = S_WIFI_INFO;
}

// ──────────────────────────────────────────────────────────────
//  Tag Tool  — standalone Seal / Unlock flow
// ──────────────────────────────────────────────────────────────
void processGen4Manage() {
  DBGLN("[GEN4] processGen4Manage: waiting for card…");
  showStatus("Tag Tool\nPlace card on\nreader");
  ledSet(0, 40, 80);  // teal

  unsigned long deadline = millis() + 20000;
  while (millis() < deadline) {
    httpServer.handleClient();
    if (touchPoll()) { enterMainMenu(); return; }
    ledScanPulse();

    // Wait for any card
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
      delay(20);
      continue;
    }

    // ── Card detected — probe Gen4 ──────────────────────────
    DBGLN("[GEN4] Card present — probing Gen4…");
    bool isG4 = gen4Detect();

    if (!isG4) {
      DBGLN("[GEN4] Not a Gen4 card — probing Gen2...");
      // Re-select after the Gen4 probe may have confused the card
      if (!rfidReSelect()) {
        DBGLN("[GEN2] re-select after Gen4 probe failed");
        ledFlash(255, 128, 0, 2);
        showStatus("Tag Tool\nCard lost after\nGen4 probe");
        unsigned long tw = millis();
        while (millis() - tw < 5000) {
          httpServer.handleClient();
          if (touchPoll()) break;
          delay(10);
        }
        enterMainMenu(); return;
      }

      uint8_t uid4[4];
      memcpy(uid4, rfid.uid.uidByte, 4);

      bool isG2 = gen2ProbeWritable(uid4);
      rfid.PCD_StopCrypto1();

      if (!isG2) {
        DBGLN("[GEN2] Not a Gen2 card — standard MIFARE");
        rfid.PICC_HaltA();
        ledFlash(255, 128, 0, 2);
        showStatus("Tag Tool\nStandard MIFARE\nBlock 0 locked\n(hardware)");
        unsigned long tw = millis();
        while (millis() - tw < 5000) {
          httpServer.handleClient();
          if (touchPoll()) break;
          delay(10);
        }
        enterMainMenu(); return;
      }

      // ── Gen2 confirmed — read current block-0 AC for header ──────────
      DBGLN("[GEN2] Gen2 confirmed — block 0 is writable");
      rfid.PICC_HaltA();
      if (!rfidReSelect()) {
        showStatus("Tag Tool\nGen2 card lost");
        appState = S_WIFI_INFO; return;
      }
      memcpy(uid4, rfid.uid.uidByte, 4);

      char g2hdr[22];
      snprintf(g2hdr, sizeof(g2hdr), "Gen2 block 0: open");
      {
        uint8_t kA[16][6], kB[16][6];
        bambuDeriveKeys(uid4, kA, kB);
        MFRC522::MIFARE_Key mA, mB, mDef;
        memcpy(mA.keyByte, kA[0], 6);
        memcpy(mB.keyByte, kB[0], 6);
        memset(mDef.keyByte, 0xFF, 6);
        if (tryAuth(3, &mB, false) || tryAuth(3, &mA, true)
         || tryAuth(3, &mDef, false) || tryAuth(3, &mDef, true)) {
          uint8_t tr[18]; uint8_t sz2 = 18;
          if (rfid.MIFARE_Read(3, tr, &sz2) == MFRC522::STATUS_OK) {
            uint8_t ab[3] = { tr[6], tr[7], tr[8] };
            uint8_t ac = mfGetAC(ab, 0);
            DBGF("[GEN2] current block 0 AC=%d\n", ac);
            if (ac == 0b010) snprintf(g2hdr, sizeof(g2hdr), "Gen2 block 0: LOCKED");
          }
        }
        rfid.PCD_StopCrypto1();
        rfid.PICC_HaltA();
      }

      // ── Show Gen2 action menu ─────────────────────────────────────────
      int g2sel = 0;
      drawGen2ActionMenu(g2sel, "Tag Tool", g2hdr);
      unsigned long t0 = millis();
      bool g2confirmed = false;
      while (millis() - t0 < 20000) {
        httpServer.handleClient();
        int tx, ty;
        if (touchGet(&tx, &ty)) {
          int bw = 350, bh = 72, gap = 12, startY = 72;
          int bx = (LCD_WIDTH - bw) / 2;
          for (int i = 0; i < 4; i++) {
            int by = startY + i * (bh + gap);
            if (tx >= bx && tx <= bx + bw && ty >= by && ty <= by + bh)
              { g2sel = i; g2confirmed = true; break; }
          }
          if (g2confirmed) break;
        }
        delay(10);
      }

      if (!g2confirmed || g2sel == 0) {
        DBGLN("[GEN2] action skipped / timeout");
        showStatus("Tag Tool\nNo action taken");
      } else {
        // Re-select for the actual operation
        if (!rfidReSelect()) {
          showStatus("Tag Tool\nCard removed");
          appState = S_WIFI_INFO; return;
        }
        memcpy(uid4, rfid.uid.uidByte, 4);

        if (g2sel == 1) {
          showStatus("Tag Tool\n\nRepairing tag...");
          bool ok = gen2RepairTag(uid4);
          DBGF("[GEN2] repair: %s\n", ok ? "OK" : "FAIL");
          rfid.PCD_StopCrypto1(); rfid.PICC_HaltA();
          if (ok) { ledFlash(0, 255, 0, 2); showStatus("Tag Tool\nTag Repaired!\nTrailer unlocked"); }
          else    { ledFlash(255, 0, 0, 2); showStatus("Tag Tool\nRepair FAILED\nCard may be\nbricked"); }
        } else if (g2sel == 2) {
          showStatus("Tag Tool\n\nLocking Block 0...");
          bool ok = gen2LockBlock0(uid4);
          DBGF("[GEN2] lock: %s\n", ok ? "OK" : "FAIL");
          rfid.PCD_StopCrypto1(); rfid.PICC_HaltA();
          if (ok) { ledFlash(0, 255, 0, 2); showStatus("Tag Tool\nBlock 0 Locked!\nRead-only now"); }
          else    { ledFlash(255, 0, 0, 2); showStatus("Tag Tool\nLock FAILED\nTrailer AC may\nblock AC writes"); }
        } else {
          showStatus("Tag Tool\n\nUnlocking Block 0...");
          bool ok = gen2UnlockBlock0(uid4);
          DBGF("[GEN2] unlock: %s\n", ok ? "OK" : "FAIL");
          rfid.PCD_StopCrypto1(); rfid.PICC_HaltA();
          if (ok) { ledFlash(0, 255, 0, 2); showStatus("Tag Tool\nBlock 0 Unlocked!\nWritable again"); }
          else    { ledFlash(255, 0, 0, 2); showStatus("Tag Tool\nUnlock FAILED\nTrailer AC may\nblock AC writes"); }
        }
      }

      unsigned long tw = millis();
      while (millis() - tw < 8000) {
          httpServer.handleClient();
          if (touchPoll()) break;
        delay(10);
      }
      enterMainMenu(); return;
    }

    // ── Gen4 confirmed — read current mode ─────────────────
    uint8_t mode = gen4GetMode();
    DBGF("[GEN4] Current mode byte: 0x%02X\n", mode);
    char hdr[32];
    if (mode == 0xFF)         snprintf(hdr, sizeof(hdr), "Gen4  mode: ???");
    else if (mode == 0x00)    snprintf(hdr, sizeof(hdr), "Gen4  SEALED (0x00)");
    else if (mode == 0x01)    snprintf(hdr, sizeof(hdr), "Gen4  mode 0x01");
    else if (mode == 0x02)    snprintf(hdr, sizeof(hdr), "Gen4  shadow (0x02)");
    else if (mode == 0x03)    snprintf(hdr, sizeof(hdr), "Gen4  magic on (0x03)");
    else                      snprintf(hdr, sizeof(hdr), "Gen4  mode 0x%02X", mode);

    int g4sel = 0;
    drawGen4ActionMenu(g4sel, "Tag Tool", hdr);

    unsigned long t0 = millis();
    bool confirmed = false;
    while (millis() - t0 < 20000) {
      httpServer.handleClient();
      int tx, ty;
      if (touchGet(&tx, &ty)) {
        int bw = 350, bh = 80, gap = 16, startY = 80;
        int bx = (LCD_WIDTH - bw) / 2;
        for (int i = 0; i < 3; i++) {
          int by = startY + i * (bh + gap);
          if (tx >= bx && tx <= bx + bw && ty >= by && ty <= by + bh)
            { g4sel = i; confirmed = true; break; }
        }
        if (confirmed) break;
      }
      delay(10);
    }

    if (confirmed && g4sel != 0) {
      if (g4sel == 1) {
        showStatus("Tag Tool\n\nSealing...");
        bool ok = gen4Seal();
        DBGF("[GEN4] Seal: %s\n", ok ? "OK" : "FAIL");
        if (ok) { showStatus("Tag Tool\nGen4 Sealed!\nMagic mode OFF\nStandard MIFARE"); ledFlash(0, 255, 0, 2); }
        else    { showStatus("Tag Tool\nGen4 Seal FAIL"); ledFlash(255, 0, 0, 2); }
      } else {  // g4sel == 2
        showStatus("Tag Tool\n\nUnlocking...");
        bool ok = gen4Unlock();
        DBGF("[GEN4] Unlock: %s\n", ok ? "OK" : "FAIL");
        if (ok) { showStatus("Tag Tool\nGen4 Unlocked!\nMagic mode ON"); ledFlash(0, 255, 0, 2); }
        else    { showStatus("Tag Tool\nGen4 Unlock FAIL\n\nNOTE: Sealed cards\ncannot be unlocked\nvia software."); ledFlash(255, 0, 0, 2); }
      }
    } else {
      DBGLN("[GEN4] Action skipped / timeout");
      showStatus("Tag Tool\nNo action taken");
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    unsigned long tw = millis();
    while (millis() - tw < 8000) {
      httpServer.handleClient();
      if (touchPoll()) break;
      delay(10);
    }
    enterMainMenu();
    return;
  }

  // Timeout
  DBGLN("[GEN4] Timeout — no card detected");
  ledFlash(255, 0, 0, 2);
  showStatus("Tag Tool\nNo card detected");
  appState = S_WIFI_INFO;  // "any key returns" state
}

void processDumpWrite() {
  DBGF("[DUMP]  processDumpWrite: file=%s  waiting for card (20 s)...\n",
       selectedDumpPath);
  // Show the dump's filament colour while waiting so the user can preview it
  TagInfo preview;
  flatToTag(dumpBuf, &preview);
  DBGF("[DUMP]  Preview: %s / %s  color=#%02X%02X%02X  wt=%.0fg\n",
       preview.filamentType, preview.detailedType,
       preview.colorR, preview.colorG, preview.colorB,
       preview.spoolWeight);
  ledSetTagColor(&preview);

  // Install sector-progress callback for both OLED-native and web-triggered flows
  g_writeSectorCb = writeProgressCbFn;
  // Show initial waiting screen
  drawWriteScreen(g_webWrite ? "Web: waiting..." : "waiting...", 0, NUM_SECTORS);

  unsigned long deadline = millis() + 20000;
  unsigned long lastDraw  = 0;
  while (millis() < deadline) {
    httpServer.handleClient();
    if (touchPoll()) {
      g_webWrite = false;
      g_writeSectorCb = nullptr;
      enterMainMenu();
      return;
    }
    // Refresh countdown every 500 ms
    if (millis() - lastDraw > 500) {
      int secsLeft = (int)((deadline - millis()) / 1000);
      char ph[24];
      snprintf(ph, sizeof(ph), g_webWrite ? "Web: wait %ds..." : "wait %ds...", secsLeft);
      drawWriteScreen(ph, 0, NUM_SECTORS);
      lastDraw = millis();
    }
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      DBGF("[DUMP]  Card detected UID: %02X %02X %02X %02X â starting write...\n",
           rfid.uid.uidByte[0], rfid.uid.uidByte[1],
           rfid.uid.uidByte[2], rfid.uid.uidByte[3]);
      ledSet(255, 255, 0);  // yellow = writing
      drawWriteScreen(g_webWrite ? "Web: writing..." : "writing...", 0, NUM_SECTORS);
      int sectOk = rfidWriteDump(dumpBuf, true);
      DBGF("[DUMP]  Write result: %d/%d sectors OK\n", sectOk, NUM_SECTORS);
      g_writeSectorCb = nullptr;
      bool ok = (sectOk == NUM_SECTORS);
      bool partial = (sectOk > 0 && sectOk < NUM_SECTORS);
      if (ok) {
        ledFlash(0, 255, 0, 3);  // 3Ã green = success
        drawWriteScreen(g_webWrite ? "Web: Done!" : "Done!", NUM_SECTORS, NUM_SECTORS);
        delay(1500);
        showStatus("Write Tag\nWrite complete!");
      } else if (partial) {
        ledFlash(255, 165, 0, 3); // 3Ã amber = partial write
        char ph2[28];
        snprintf(ph2, sizeof(ph2), g_webWrite ? "Web: %d/16 partial" : "%d/16 partial", sectOk);
        drawWriteScreen(ph2, sectOk, NUM_SECTORS); delay(1500);
        char msg[64];
        snprintf(msg, sizeof(msg), "Partial! %d/16 sec\nCard keyed wrong?", sectOk);
        showStatus((String("Write Tag\n") + msg).c_str());
      } else {
        ledFlash(255, 0, 0, 3);  // 3Ã red = fail
        drawWriteScreen(g_webWrite ? "Web: FAILED!" : "FAILED!", 0, NUM_SECTORS);
        delay(1500);
        showStatus("Write Tag\nWrite failed!\nTry a magic/FUID\ncard.");
      }
      g_webWrite = false;
      appState = S_WIFI_INFO;
      return;
    }
    delay(18);
  }
  DBGLN("[DUMP]  processDumpWrite: timeout â no card.");
  ledFlash(255, 0, 0, 2);
  drawWriteScreen(g_webWrite ? "Web: timeout!" : "timeout!", 0, NUM_SECTORS);
  delay(1500);
  showStatus("Write Tag\nTimeout. No card.");
  g_webWrite = false;
  g_writeSectorCb = nullptr;
  appState = S_WIFI_INFO;
}
void setup() {
  Serial.begin(115200);
  delay(100);

  DBGLN("\n\n========================================");
  DBGLN("  BambuTagger  – debug build");
  DBGLN("  Compiled: " __DATE__ "  " __TIME__);
  DBGLN("========================================");
  DBGF("PSRAM: total=%u free=%u\n", ESP.getPsramSize(), ESP.getFreePsram());
  Serial.flush();
  DBGLN("setup: after PSRAM"); Serial.flush();

  // ── Touchscreen display (ST7262 RGB + GT911) ──────────
  DBGLN("backlight..."); Serial.flush();
  pinMode(2, OUTPUT); digitalWrite(2, HIGH);
  DBGLN("backlight done"); Serial.flush();
  DBGLN("LCD init..."); Serial.flush();
  bool ok = lcd.init();
  DBGF("LCD init=%d\n", ok); Serial.flush();
  DBGLN("LCD setBrightness..."); Serial.flush();
  lcd.setBrightness(255);
  DBGLN("LCD setRotation..."); Serial.flush();
  lcd.setRotation(0);
  DBGLN("draw splash..."); Serial.flush();
  bool jpg = lcd.drawJpg(splash_jpg, splash_jpg_len, 0, 0);
  DBGF("jpg=%d\n", jpg); Serial.flush();
  DBGLN("splash delay..."); Serial.flush();
  delay(1500);
  DBGLN("splash done");

  // ── Touch calibration (first boot) ─────────────────────
  {
    uint16_t calParams[8];
    if (touchCalLoad(calParams)) {
      lcd.setTouchCalibrate(calParams);
      DBGLN("touch cal restored");
    } else {
      DBGLN("touch cal: run calibration...");
      lcd.fillScreen(TFT_BLACK);
      lcd.setTextColor(TFT_WHITE, TFT_BLACK);
      lcd.setTextDatum(MC_DATUM);
      lcd.setTextSize(2);
      lcd.drawString("Tap each target to calibrate touch", LCD_WIDTH / 2, LCD_HEIGHT / 2);
      delay(1500);
      lcd.calibrateTouch(calParams, TFT_RED, TFT_BLACK, 10);
      touchCalSave(calParams);
      DBGLN("touch cal saved");
    }
  }

  // ── SPI / RC522 (JC8048W550 HSPI bus) ──────────────────
  pinMode(PIN_RFID_CS, OUTPUT); digitalWrite(PIN_RFID_CS, HIGH);
  rfidSPI.begin(12, 13, 11, 18);   // SCK=12, MISO=13, MOSI=11, SS/RFID_CS=18
  pinMode(PIN_RFID_RST, OUTPUT); digitalWrite(PIN_RFID_RST, LOW); delay(50);
  digitalWrite(PIN_RFID_RST, HIGH); delay(50);
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  Serial.print(F("RC522 firmware: "));
  rfid.PCD_DumpVersionToSerial();

  // ── FFat ─────────────────────────────────────────────
  if (!FFat.begin(true))
    Serial.println(F("FFat mount failed!"));
  else
    Serial.printf("FFat: %u/%u bytes used",
                  FFat.usedBytes(), FFat.totalBytes());

  // ── WiFi ────────────────────────────────────────────────
  wifiLoadCreds();
  bool connected = wifiConnect();
  if (connected) {
    apMode = false;
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
  } else {
    wifiStartAP();
  }

  // ── HTTP server ──────────────────────────────────────────
  setupHTTPServer();

  // ── Show main menu ───────────────────────────────────────
  delay(200);
  DBGLN("drawMenu...");
  drawMenu();
  DBGLN("setup done");
}

// ──────────────────────────────────────────────────────────────
//  Tap-action processors for browsers (called directly from handleTouch)
// ──────────────────────────────────────────────────────────────
static void fatDeleteCurrent() {
  char msg[128];
  snprintf(msg, sizeof(msg), "Delete empty folder?\n%s", fatCurPath.c_str());
  showStatus((String("Write Tag\n") + msg).c_str());
  unsigned long deadline = millis() + 10000;
  while (millis() < deadline) {
    httpServer.handleClient();
    int tx, ty;
    if (touchGet(&tx, &ty)) {
      while (touchGet(&tx, &ty)) { delay(10); httpServer.handleClient(); }
      bool ok = FFat.remove(fatCurPath.c_str());
      if (ok) {
        fatDepth--; fatCurPath = fatDirStack[fatDepth];
        fatLoadDir(fatCurPath); drawFatBrowser();
      } else {
        showStatus("Write Tag\nDelete failed");
        while (!touchPoll()) { delay(10); httpServer.handleClient(); }
        enterMainMenu();
      }
      return;
    }
    delay(10);
  }
  showStatus("Write Tag\nTimeout");
  while (!touchPoll()) { delay(10); httpServer.handleClient(); }
  enterMainMenu();
}

static void processFatBrowserTap() {
  if (fatSel == 0 && fatDepth > 0) {
    fatDepth--; fatCurPath = fatDirStack[fatDepth];
    fatLoadDir(fatCurPath); drawFatBrowser();
    return;
  }
  int ei = (fatDepth > 0) ? fatSel - 1 : fatSel;
  if (fatEntries[ei].isDir) {
    if (fatDepth < FAT_MAX_DEPTH) fatDirStack[fatDepth++] = fatCurPath;
    fatCurPath = (fatCurPath == "/") ? String("/") + fatEntries[ei].name
                                     : fatCurPath + "/" + fatEntries[ei].name;
    fatLoadDir(fatCurPath); drawFatBrowser();
    return;
  }
  String fullPath = (fatCurPath == "/") ? String("/") + fatEntries[ei].name
                                        : fatCurPath + "/" + fatEntries[ei].name;
  strncpy(selectedDumpPath, fullPath.c_str(), sizeof(selectedDumpPath) - 1);
  File f = FFat.open(fullPath, FILE_READ);
  if (!f || f.size() != DUMP_SIZE) {
    showStatus("Browser\nBad tag file!");
    appState = S_WIFI_INFO; return;
  }
  f.read(dumpBuf, DUMP_SIZE); f.close();
  TagInfo preview; flatToTag(dumpBuf, &preview);
  char msg[128];
  snprintf(msg, sizeof(msg), "Write tag:%s\n%s\n#%0lX%0lX%0lX\n\nPlace blank card",
           preview.filamentType, preview.detailedType,
           preview.colorR, preview.colorG, preview.colorB);
  showStatus((String("Browser\n") + msg).c_str()); appState = S_DUMP_WRITE;
}
static void processGhBrowseTap() {
  int headerRows = (ghDepth == 0) ? 1 : 2;
  int totalRows = ghCount + headerRows;
  if (ghSel == 0) {
    if (ghDepth == 0) { enterMainMenu(); return; }
    ghDepth--; ghSel = 0; ghScroll = 0;
    String parentPath = (ghDepth > 0) ? ghStack[ghDepth - 1] : "";
    showStatus2("Github Library", "Loading"); ghFetchDir(parentPath); drawGhBrowser();
    return;
  }
  if (ghSel == 1 && ghDepth > 0) { enterMainMenu(); return; }
  int eIdx = ghSel - headerRows;
  if (eIdx < 0 || eIdx >= ghCount) return;
  GhEntry& entry = ghEntries[eIdx];
  if (entry.isDir) { enterGhBrowse(String(entry.path), true); return; }
  String fname = String(entry.name);
  if (!fname.endsWith(".bin") && !fname.endsWith(".json")) {
    showStatus2("Skip file", fname.substring(0, 16).c_str());
    delay(1500); drawGhBrowser(); return;
  }
  String rawUrl = GITHUB_RAW_PREFIX + String(entry.path);
  String localName = buildDumpFilePath(String(entry.path));
  appState = S_GH_DOWNLOAD;
  ledSet(255, 200, 0);
  lcd.fillScreen(TFT_BLACK); drawStatusBar(); drawSubHeader("Downloading...");
  lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(2);
  lcd.setCursor(10, 138);
  String shortName = localName;
  if (shortName.length() > 30) shortName = shortName.substring(0, 29) + "~";
  lcd.print(shortName);
  bool ok = ghSaveFile(rawUrl, localName);
  if (ok) {
    ledFlash(0, 255, 0, 3);
    ghDlStatus = "Saved: " + localName;
    fatNavigateTo(localName);
    File df = FFat.open(localName, FILE_READ);
    if (df && (int)df.size() == DUMP_SIZE) {
      df.read(dumpBuf, DUMP_SIZE); df.close();
      strncpy(selectedDumpPath, localName.c_str(), sizeof(selectedDumpPath) - 1);
      TagInfo preview; flatToTag(dumpBuf, &preview);
      char msg[128];
      snprintf(msg, sizeof(msg), "GitHub OK!\n%.16s\n%.16s",
               preview.filamentType, preview.detailedType);
      showStatus((String("GitHub Library") + msg).c_str());
      unsigned long deadline = millis() + 15000;
      while (millis() < deadline) {
        httpServer.handleClient();
        if (touchPoll()) { appState = S_DUMP_WRITE; return; }
        delay(20);
      }
    } else {
      if (df) df.close();
      showStatus2("Downloaded!", ("Use WriteDump\n" + shortDumpName(localName)).c_str());
      delay(3000);
    }
  } else {
    ledFlash(255, 0, 0, 3);
    ghDlStatus = "Failed!";
    showStatus2("Download FAILED", "See Serial");
    delay(3000);
  }
  appState = S_GH_BROWSE;
  String curPath = (ghDepth > 0) ? ghStack[ghDepth - 1] : "";
  ghFetchDir(curPath); ghSel = 0; ghScroll = 0;
  ledSet(0, 0, 40); drawGhBrowser();
}
static void processBmCatBrowseTap() {
  if (bmCatSel == 0 && bmCatLevel > 0) {
    int prev = bmCatLevel - 1;
    if (prev <= 0) { bmCatMat[0] = '\0'; bmCatType[0] = '\0'; bmCatColor[0] = '\0'; }
    if (prev <= 1) { bmCatType[0] = '\0'; bmCatColor[0] = '\0'; }
    if (prev <= 2) { bmCatColor[0] = '\0'; }
    enterBmCatBrowse(prev); return;
  }
  int eIdx = bmCatSel - ((bmCatLevel > 0) ? 1 : 0);
  if (eIdx < 0 || eIdx >= bmCatCount) return;
  const char* sel = bmCatEntries[eIdx].label;
  if (bmCatLevel == 0) { strncpy(bmCatMat, sel, 31); bmCatMat[31] = '\0'; enterBmCatBrowse(1); }
  else if (bmCatLevel == 1) { strncpy(bmCatType, sel, 31); bmCatType[31] = '\0'; enterBmCatBrowse(2); }
  else if (bmCatLevel == 2) { strncpy(bmCatColor, sel, 31); bmCatColor[31] = '\0'; enterBmCatBrowse(3); }
  else {
    String uid = String(sel);
    String saved = bmCatFetchUid(uid);
    if (saved.isEmpty()) {
      unsigned long t0 = millis();
      while (millis() - t0 < 10000) {
        httpServer.handleClient();
        if (touchPoll()) break;
        delay(20);
      }
      enterBmCatBrowse(3); return;
    }
    File df = FFat.open(saved, FILE_READ);
    if (df && df.size() == DUMP_SIZE) {
      df.read(dumpBuf, DUMP_SIZE); df.close();
      strncpy(selectedDumpPath, saved.c_str(), sizeof(selectedDumpPath) - 1);
      TagInfo preview; flatToTag(dumpBuf, &preview);
      char msg[128];
      snprintf(msg, sizeof(msg), "BambuMan OK!\n%.16s\n%.16s",
               preview.filamentType, preview.detailedType);
      showStatus((String("BambuMan Library\n") + msg).c_str()); 
      ledFlash(0, 255, 0, 2);
      unsigned long deadline = millis() + 15000;
      while (millis() < deadline) {
        httpServer.handleClient();
        if (touchPoll()) { appState = S_DUMP_WRITE; return; }
        delay(20);
      }
    } else {
      if (df) df.close();
      showStatus(("BambuMan Library\nSaved!\n" + saved).c_str());
      ledFlash(0, 255, 0, 2);
      unsigned long t0 = millis();
      while (millis() - t0 < 8000) {
        httpServer.handleClient();
        if (touchPoll()) break;
        delay(20);
      }
    }
    enterBmCatBrowse(3);
  }
}

// ──────────────────────────────────────────────────────────────
//  Touch-drag scroll helpers for browsers
// ──────────────────────────────────────────────────────────────
static void scrollFatBrowser(int dir) {
  int total = fatTotalRows();
  int visRows = LIST_MAX_VIS;
  if (total <= visRows) return;
  fatScroll = constrain(fatScroll + dir, 0, total - visRows);
  fatSel = constrain(fatSel, fatScroll, fatScroll + visRows - 1);
  drawFatBrowser();
}
static void scrollGhBrowse(int dir) {
  int headerRows = (ghDepth == 0) ? 1 : 2;
  int totalRows = ghCount + headerRows;
  if (totalRows <= LIST_MAX_VIS) return;
  ghScroll = constrain(ghScroll + dir, 0, totalRows - LIST_MAX_VIS);
  ghSel = constrain(ghSel, ghScroll, ghScroll + LIST_MAX_VIS - 1);
  drawGhBrowser();
}
static void scrollBmCatBrowse(int dir) {
  int backExtra = (bmCatLevel > 0) ? 1 : 0;
  int totalRows = bmCatCount + backExtra;
  int visRows = LIST_MAX_VIS;
  if (bmCatLevel == 0) visRows = min(LIST_MAX_VIS, (FOOTER_Y - 196) / LIST_ROW_H);
  if (totalRows <= visRows) return;
  bmCatScroll = constrain(bmCatScroll + dir, 0, totalRows - visRows);
  bmCatSel = constrain(bmCatSel, bmCatScroll, bmCatScroll + visRows - 1);
  drawBmCatBrowser();
}

// ──────────────────────────────────────────────────────────────
//  Touch input handler – drag-scroll in browsers, tap everywhere
// ──────────────────────────────────────────────────────────────
static void handleTouch() {
  static int touchStartX = -1, touchStartY = -1;
  static int lastTouchY = -1;
  static int dragAccum = 0;
  static bool dragOccurred = false;
  static uint32_t touchStartMs = 0;
  static bool consumed = false;
  static uint32_t fingerUpMs = 0;

  int tx, ty;
  if (!touchGet(&tx, &ty)) {
    // Finger lifted — process tap if no drag happened
    if (touchStartX >= 0 && !dragOccurred && !consumed) {
      fingerUpMs = millis();
      consumed = true;
      int ttx = touchStartX, tty = touchStartY;
      if (tty < 64 && appState != S_MAIN_MENU) { enterMainMenu(); touchStartX = -1; return; }
      switch (appState) {
        case S_MAIN_MENU: {
          int col = (ttx - MENU_X0) / (BTN_W + BTN_GAP_X);
          int row = (tty - MENU_Y0) / (BTN_H + BTN_GAP_Y);
          int idx = row + col * MENU_ROWS;
          if (idx >= 0 && idx < MENU_COUNT && ttx >= MENU_X0 && tty >= MENU_Y0 &&
              col < MENU_COLS && row < MENU_ROWS) {
            int x0 = MENU_X0 + col * (BTN_W + BTN_GAP_X);
            int y0 = MENU_Y0 + row * (BTN_H + BTN_GAP_Y);
            if (ttx >= x0 && ttx <= x0 + BTN_W && tty >= y0 && tty <= y0 + BTN_H) {
              menuSel = idx;
              menuScroll = (idx < 2) ? 0 : idx - 1;
              drawMenu();
              switch (idx) {
                case 0: enterReadTag(); break;
                case 1: enterCloneSource(); break;
                case 2: enterFatBrowser(); break;
                case 3: ghDepth = 0; enterGhBrowse("", true); break;
                case 4: enterBmBrowse(); break;
                case 5: appState = S_GEN4_MANAGE; break;
                case 6: enterWifiInfo(); break;
                case 7: appState = S_OTA_UPDATE; break;
              }
            }
          }
          break;
        }
        case S_SHOW_TAG:
          break;
        case S_DUMP_SELECT: {
          if (sbTapScroll(ttx, tty, fatTotalRows(), fatScroll, SB_Y, SB_H)) { drawFatBrowser(); break; }
          if (fatCount == 0 && fatDepth > 0) {
    int delY = LIST_ROW_Y0 + LIST_ROW_H;
            if (ttx >= 8 && ttx <= 8 + LIST_BTN_W && tty >= delY && tty <= delY + LIST_BTN_H)
              { fatDeleteCurrent(); break; }
          }
          int total = fatTotalRows();
          int scroll = fatScroll;
          for (int i = 0; i < LIST_MAX_VIS && (scroll + i) < total; i++) {
            int by = LIST_ROW_Y0 + i * LIST_ROW_H;
            if (ttx >= 8 && ttx <= 8 + LIST_BTN_W && tty >= by && tty <= by + LIST_BTN_H) {
              int rowIdx = scroll + i;
              if (rowIdx < total) { fatSel = rowIdx; processFatBrowserTap(); }
              break;
            }
          }
          break;
        }
        case S_WIFI_INFO:
          // Delete All Tags button: x=200,w=400, y=360,h=56
          if (ttx >= 200 && ttx <= 600 && tty >= 360 && tty <= 416) {
            lcd.fillScreen(TFT_BLACK);
            drawStatusBar(); drawSubHeader("System");
            lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(3);
            lcd.setTextDatum(MC_DATUM);
            lcd.drawString("Delete ALL tags?", LCD_WIDTH / 2, 160);
            lcd.drawString("This cannot be undone!", LCD_WIDTH / 2, 200);
            lcd.setTextSize(2);
            drawBtn(200, 280, 400, 56, TFT_MAROON, "Yes, delete all");
            drawBtn(200, 350, 400, 56, TFT_DARKGREY, "Cancel");
            drawFooter(); lcd.display();
            // Wait for confirmation
            unsigned long dd = millis() + 10000;
            bool confirmed = false;
            while (millis() < dd) {
              int tx2, ty2;
              if (touchGet(&tx2, &ty2)) {
                while (touchGet(&tx2, &ty2)) { delay(10); }
                if (tx2 >= 200 && tx2 <= 600 && ty2 >= 280 && ty2 <= 336) {
                  confirmed = true; break;
                }
                if (tx2 >= 200 && tx2 <= 600 && ty2 >= 350 && ty2 <= 406) break;
                if (ty2 < 64) break;  // header tap cancels
              }
              delay(20);
            }
            if (confirmed) {
              int cnt = 0;
              deleteAllDumpsRecursive("/", cnt);
              char buf[32];
              snprintf(buf, sizeof(buf), "Deleted %d tags", cnt);
              showStatus(buf);
              delay(2000);
            }
            enterWifiInfo();
          }
          break;
        case S_GH_BROWSE: {
          int headerRows = (ghDepth == 0) ? 1 : 2;
          int totalRows = ghCount + headerRows;
          if (sbTapScroll(ttx, tty, totalRows, ghScroll, SB_Y, SB_H)) break;
          for (int i = 0; i < LIST_MAX_VIS; i++) {
            int idx = ghScroll + i;
            if (idx >= totalRows) break;
            int by = LIST_ROW_Y0 + i * LIST_ROW_H;
            if (ttx >= 8 && ttx <= 8 + LIST_BTN_W && tty >= by && tty <= by + LIST_BTN_H)
              { ghSel = idx; processGhBrowseTap(); break; }
          }
          break;
        }
        case S_BM_CAT_BROWSE: {
          // Sync buttons at level 0
          if (bmCatLevel == 0) {
            int btnY = 140;
            if (tty >= btnY && tty <= btnY + 46) {
              if (ttx >= 8 && ttx <= 392) { bmOledSyncCatalogQuick(); break; }
              if (ttx >= 400 && ttx <= 784) { bmOledSyncCatalog(); break; }
            }
          }
          int backExtra = (bmCatLevel > 0) ? 1 : 0;
          int totalRows = bmCatCount + backExtra;
          int listY0 = (bmCatLevel == 0) ? 196 : LIST_ROW_Y0;
          int visRows = LIST_MAX_VIS;
          if (bmCatLevel == 0) visRows = min(LIST_MAX_VIS, (FOOTER_Y - listY0) / LIST_ROW_H);
          if (sbTapScroll(ttx, tty, totalRows, bmCatScroll, listY0, visRows * LIST_ROW_H)) break;
          for (int i = 0; i < visRows; i++) {
            int idx = bmCatScroll + i;
            if (idx >= totalRows) break;
            int by = listY0 + i * LIST_ROW_H;
            if (ttx >= 8 && ttx <= 8 + LIST_BTN_W && tty >= by && tty <= by + LIST_BTN_H)
              { bmCatSel = idx; processBmCatBrowseTap(); break; }
          }
          break;
        }
      }
    }
    touchStartX = -1;
    return;
  }

  // Stale-touch guard: if too long since last touch cycle, reset
  if (touchStartX >= 0 && millis() - touchStartMs > 500) {
    touchStartX = -1;
  }

  if (touchStartX < 0) {
    if (fingerUpMs && millis() - fingerUpMs < 50) return;
    consumed = false;
    touchStartX = tx; touchStartY = ty;
    lastTouchY = ty;
    dragAccum = 0; dragOccurred = false;
    touchStartMs = millis();
    return;
  }

  // Continuous touch — detect drag-scroll in browser states
  if (appState != S_DUMP_SELECT && appState != S_GH_BROWSE && appState != S_BM_CAT_BROWSE)
    return;

  // Scrollbar tap-to-jump (also catches initial touch on scrollbar)
  if (!dragOccurred) {
    if (appState == S_DUMP_SELECT) {
      if (sbTapScroll(tx, ty, fatTotalRows(), fatScroll, SB_Y, SB_H)) { drawFatBrowser(); dragOccurred = true; return; }
    } else if (appState == S_GH_BROWSE) {
      int hr = (ghDepth == 0) ? 1 : 2;
      if (sbTapScroll(tx, ty, ghCount + hr, ghScroll, SB_Y, SB_H)) { drawGhBrowser(); dragOccurred = true; return; }
    } else if (appState == S_BM_CAT_BROWSE) {
      int backExtra = (bmCatLevel > 0) ? 1 : 0;
      int totalRows = bmCatCount + backExtra;
      int listY0 = (bmCatLevel == 0) ? 196 : LIST_ROW_Y0;
      int vr = LIST_MAX_VIS;
      if (bmCatLevel == 0) vr = min(LIST_MAX_VIS, (FOOTER_Y - listY0) / LIST_ROW_H);
      if (sbTapScroll(tx, ty, totalRows, bmCatScroll, listY0, vr * LIST_ROW_H)) { drawBmCatBrowser(); dragOccurred = true; return; }
    }
  }

  int delta = ty - lastTouchY;
  lastTouchY = ty;
  dragAccum += delta;
  if (abs(dragAccum) >= 30) {
    int dir = (dragAccum > 0) ? 1 : -1;
    dragAccum = 0;
    dragOccurred = true;
    if (appState == S_DUMP_SELECT) scrollFatBrowser(dir);
    else if (appState == S_GH_BROWSE) scrollGhBrowse(dir);
    else if (appState == S_BM_CAT_BROWSE) scrollBmCatBrowse(dir);
  }
}

// ──────────────────────────────────────────────────────────────
//  Arduino loop()
// ──────────────────────────────────────────────────────────────
static uint32_t loopCount = 0;
void loop() {
  httpServer.handleClient();
  handleTouch();

  switch (appState) {

    // ── Main menu – handled by touch ──────────────────────
    case S_MAIN_MENU:
    case S_SHOW_TAG:
    case S_WIFI_INFO:
      // handled by handleTouch() above
      break;

    // ── Select dump file from FAT ─────────────────────
    case S_DUMP_SELECT:
      // handled by handleTouch() via processFatBrowserTap()
      break;

    // ── Active RFID operations (enter blocking loop once) ─
    case S_READ_TAG:
      processReadTag();
      break;

    case S_CLONE_SOURCE:
      processCloneSource();
      break;

    case S_CLONE_TARGET:
      processCloneTarget();
      break;

    case S_DUMP_WRITE:
      processDumpWrite();
      break;

    case S_GH_BROWSE:
    case S_GH_DOWNLOAD:
      // handled by handleTouch() via processGhBrowseTap()
      break;

    // ── BambuMan fetch ────────────────────────────────────────
    case S_BM_CAT_BROWSE:
      // handled by handleTouch() via processBmCatBrowseTap()
      break;

    case S_BM_BROWSE:
    case S_BM_DOWNLOAD:
      processBmBrowse();
      break;

    case S_OTA_UPDATE:
      processOtaUpdate();
      break;

    case S_GEN4_MANAGE:
      processGen4Manage();
      break;

    default:
      enterMainMenu();
      break;
  }
}
