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
#include "config.h"
#include "ui/splash_jpg.h"
#include "ui/logo_bitmap.h"
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LovyanGFX.hpp>
#include <functional>
#include <rom/miniz.h>

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
// Pin config in config.h (PIN_RFID_CS, PIN_RFID_RST)

// ──────────────────────────────────────────────────────────────
//  Constants
// ──────────────────────────────────────────────────────────────
// (Most constants live in config.h; only internal code values here.)

// Bambu Lab HKDF salt (from reverse-engineered KDF)
static const uint8_t BAMBU_KDF_SALT[16] = {
  0x9a, 0x75, 0x9c, 0xf2, 0xc4, 0xf7, 0xca, 0xff,
  0x22, 0x2c, 0xb9, 0x76, 0x9b, 0x41, 0xbc, 0x96
};



// ──────────────────────────────────────────────────────────────
//  Tag source identifier
// ──────────────────────────────────────────────────────────────
enum TagSource {
  TAG_SRC_BAMBU,
  TAG_SRC_TIGERTAG,
  TAG_SRC_OPENSPOOL,
  TAG_SRC_OPENTAG3D,
  TAG_SRC_SPOOLEASE,
  TAG_SRC_UNKNOWN_NTAG,
  TAG_SRC_UNKNOWN
};

// ──────────────────────────────────────────────────────────────
//  Tag data
// ──────────────────────────────────────────────────────────────
struct TagInfo {
  uint8_t uid[4];
  char filamentType[17];  // block 2
  char detailedType[17];  // block 4
  char variantId[32];     // block 1 bytes 0-7 (Bambu) / brand name (others)
  char materialId[32];    // block 1 bytes 8-15 (Bambu) / material label (others)
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
  TagSource tagSource;    // which format was decoded
  char tagUrl[128];       // URL for SpoolEase tags (others: empty)
  bool valid;
};

// ── NDEF record parser ─────────────────────────────────────────
struct NdefRecord {
  uint8_t tnf;
  char    type[64];
  uint8_t payload[NDEF_MAX_PAYLOAD + 1];
  int     payloadLen;
};

TagInfo currentTag;  // most recently read
TagInfo sourceTag;   // for clone operation
uint8_t dumpBuf[DUMP_SIZE];
uint8_t ntagWriteBuf[192];  // NTAG page data for TigerTag write (48 pages × 4)
int     ntagWritePages = 0;                 // number of pages in ntagWriteBuf
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
uint16_t* logoBuffer = nullptr;  // pre‑composed logo (transparent→COL_SIDEBAR)
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

void bambuDeriveKeys(const uint8_t uid[4],
                     uint8_t keysA[16][6],
                     uint8_t keysB[16][6]) {
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
//  Forward declarations (needed since we compile as .cpp, no .ino auto-prototyping)
// ──────────────────────────────────────────────────────────────
static bool rfidReSelect();
static bool touchPoll();
static bool touchGet(int* tx, int* ty);
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
//  NTAG (NFC Type 2) tag support — SpoolEase · OpenSpool · TigerTag
// ──────────────────────────────────────────────────────────────

// ── TigerTag API database lookup tables ───────────────────────
struct TigerTagEntry { uint32_t id; const char* label; };
struct TigerTagMat   { uint32_t id; const char* label; const char* matType; const char* filledType; };

static const TigerTagMat ttMaterials[] = {
  {18775, "PE-CF", "PE", "CF"},
  {34944, "PETG-GF", "", ""},
  {9691, "EVA", "EVA", ""},
  {49804, "ASA-AF", "ASA", "AF"},
  {7951, "PETG-rCF", "PETG", "CF"},
  {11053, "PET-CF", "PET", "CF"},
  {53890, "PCTG-CF", "PCTG", "CF"},
  {51636, "PES", "", ""},
  {18130, "PS", "PS", ""},
  {9456, "PLA Marble", "PLA", ""},
  {27676, "ASA-CF", "ASA", "CF"},
  {30594, "PA-GF", "PA", "GF"},
  {58142, "TPU-GF", "TPU", "GF"},
  {24270, "PPS-CF", "PPS", "CF"},
  {48001, "PLA Wood", "PLA", "Wood"},
  {56527, "PEI-1010", "PEI-1010", ""},
  {24116, "TPC", "TPC", ""},
  {57469, "PETG HF", "PETG", ""},
  {46591, "PLA+", "PLA", ""},
  {425, "ABS-CF", "ABS", "CF"},
  {42962, "PP-GF", "PP", "GF"},
  {10122, "PEKK-CF", "", ""},
  {27268, "PCTPE", "PCPTFE", ""},
  {59328, "PA", "PA", ""},
  {18703, "PETP", "PET", ""},
  {33958, "TPE", "TPE", ""},
  {48310, "PLA-CF", "PLA", "CF"},
  {31011, "ASA-AERO", "ASA", "AERO"},
  {27635, "PE", "PE", ""},
  {42623, "PMMA", "PMMA", ""},
  {24629, "PLA High Speed", "PLA", ""},
  {65535, "None", "None", ""},
  {3368, "PC-ABS", "PC", ""},
  {50206, "POM", "POM", ""},
  {43518, "TPU", "TPU", ""},
  {7649, "PETG HS", "PETG", ""},
  {45962, "PVB", "PVB", ""},
  {12216, "PEEK-GF", "", ""},
  {30884, "PP", "PP", ""},
  {39944, "PA-CF", "PA", "CF"},
  {1173, "PA6-GF", "PA6", "GF"},
  {62335, "PEI-1010-CF", "PEI-1010", "CF"},
  {51007, "Biopolymer", "", ""},
  {8394, "Castable Resin", "", ""},
  {24115, "SEBS", "SEBS", ""},
  {56666, "PA6", "PA6", ""},
  {48469, "PEEK-CF", "", ""},
  {35100, "ASA-GF", "ASA", "GF"},
  {14508, "PEI-9085", "", ""},
  {5238, "PETG-PTFE", "PETG", ""},
  {54568, "ASA+", "ASA", ""},
  {9483, "PVA", "PVA", ""},
  {10272, "PSU", "PSU", ""},
  {41134, "PEI-9085-CF", "", ""},
  {12844, "ASA", "ASA", ""},
  {63946, "TPI", "TPI", ""},
  {55279, "PBT", "PBT", ""},
  {20073, "PVC", "PVC", ""},
  {21307, "SBS", "SBS", ""},
  {8504, "PPA-CF", "PPA", "CF"},
  {35377, "PEI-9085-GF", "", ""},
  {38250, "PEI-1010-GF", "", ""},
  {29815, "PEEK", "PEEK", ""},
  {38256, "PETG", "PETG", ""},
  {12878, "CoPE", "", ""},
  {55418, "PETG-CF", "PETG", "CF"},
  {61563, "PC-PBT-GF", "PC", "GF"},
  {34409, "TPS", "TPS", ""},
  {1680, "PE-GF", "", ""},
  {59849, "PCL", "", ""},
  {20588, "PC-CF", "", ""},
  {15041, "PCTG", "PCTG", ""},
  {46154, "PPS", "PPS", ""},
  {3481, "PCTG-GF", "PCTG", "GF"},
  {46276, "PPA-GF", "PPA", "GF"},
  {23080, "PAHT", "", ""},
  {52077, "PET", "PET", ""},
  {22652, "PAHT-GF", "", ""},
  {5733, "TPU-AMS", "TPU", ""},
  {38219, "PLA", "PLA", ""},
  {22678, "PET-GF", "PET", "GF"},
  {2053, "PA12-GF", "PA12", "GF"},
  {12264, "PA6-CF", "PA6", "CF"},
  {29272, "PA11", "", ""},
  {51861, "PETG-ESD", "PETG", ""},
  {47651, "PC-PBT-CF", "PC", "CF"},
  {20562, "ABS", "ABS", ""},
  {53970, "PEKK", "PEKK", ""},
  {18451, "PA11-CF", "", ""},
  {6605, "PA11-GF", "", ""},
  {48815, "PAHT-CF", "PAHT", "CF"},
  {39667, "PA12-CF", "PA12", "CF"},
  {49074, "ABS-GF", "ABS", "GF"},
  {55796, "PA12", "PA12", ""},
  {58498, "PEBA", "PEBA", ""},
  {61048, "PVDF", "PVDF", ""},
  {26029, "HIPS", "HIPS", ""},
  {8345, "PLA+ Silk", "PLA", ""},
  {13850, "PPA", "PPA", ""},
  {10738, "PC-PTFE", "PC", "PTFE"},
  {4587, "PC-PBT", "PC", ""},
  {10187, "PHA", "PHA", ""},
  {735, "ABS-AF", "ABS", "AF"},
  {28110, "SBC", "SBC", ""},
  {50497, "PP-CF", "PP", "CF"},
  {30458, "PC", "PC", ""},
  {10478, "Castable Filament", "", ""},
  {48047, "TPU High Speed", "TPU", ""},
  {18922, "PLA-ESD", "PLA", ""},
  {11506, "PLA-AERO", "PLA", "AERO"},
  {49152, "PPSU", "PPSU", ""},
  {10602, "PLA Silk", "PLA", ""},
  {34049, "BVOH", "BVOH", ""},
};
static const int ttMaterialsCount = 113;

static const TigerTagEntry ttBrands[] = {
  {1, "Atome3D"},
  {1068, "SainSmart"},
  {1120, "Proto-Pasta"},
  {1421, "3DJake"},
  {2517, "Smart Materials 3D"},
  {2833, "Xstrand"},
  {3132, "Hatchbox"},
  {3924, "FiloAlfa"},
  {4011, "QIDI Tech"},
  {4048, "Owa"},
  {4344, "MatterHackers"},
  {4356, "Landu"},
  {4565, "Valment"},
  {4700, "Filforme"},
  {6305, "Markforged"},
  {7432, "Aliz"},
  {7674, "Extrudr"},
  {7812, "Jayo"},
  {7980, "Fillamentum"},
  {8182, "Fiberlogy"},
  {8303, "GST3D"},
  {8384, "Taulman3D"},
  {8496, "CaiLab"},
  {8586, "NinjaTek"},
  {8675, "SOVB 3D"},
  {8756, "BlueCast"},
  {8921, "Duramic 3D"},
  {8990, "Ice Filaments"},
  {9192, "3D Solutech"},
  {9299, "FDPlast"},
  {9394, "Gizmo Dorks"},
  {9596, "Ziro"},
  {9798, "AMOLEN"},
  {11379, "Filaments.ca"},
  {11429, "3D4Makers"},
  {11501, "InnovateFil"},
  {12345, "MakerBot"},
  {12498, "Forshape"},
  {12635, "Snapmaker"},
  {13667, "RS Pro"},
  {14250, "Jessie (Printed Solid)"},
  {14982, "3D-Fuel"},
  {15899, "Kimya"},
  {15962, "Anycubic"},
  {18629, "PrintoMax 3D"},
  {19265, "CC3D"},
  {19961, "Rosa3D"},
  {20523, "Raise3D"},
  {20851, "Tronxy"},
  {22652, "Spectrum"},
  {23181, "ArianePlast"},
  {23456, "Monoprice"},
  {24363, "Tecbears"},
  {26379, "Formlabs"},
  {26595, "Sovol"},
  {26956, "Creality"},
  {28055, "TAGin3D"},
  {28136, "Polar Filament"},
  {28940, "Eryone"},
  {28988, "KINGROON"},
  {29045, "Yousu"},
  {29302, "IIIDMAX"},
  {31438, "Wondermaker"},
  {32348, "Addnorth"},
  {32587, "Amazon"},
  {33566, "Siraya Tech"},
  {33594, "DEEPLEE"},
  {33788, "Verbatim"},
  {34567, "Push Plastic"},
  {34597, "EconoFil"},
  {35123, "Bambu Lab"},
  {35501, "Zortrax"},
  {35857, "Nobufil"},
  {35882, "Phrozen"},
  {36702, "Tianse"},
  {37434, "Winkle"},
  {38533, "Lotactree"},
  {39002, "GreenGate3D"},
  {39382, "Longer"},
  {39652, "3DXTech"},
  {41847, "OneFil"},
  {41932, "Jamg He"},
  {42911, "UltiMaker"},
  {44630, "NIT"},
  {45670, "Panchroma"},
  {45678, "Atomic Filament"},
  {46010, "AceAddity"},
  {46203, "Overture"},
  {46392, "Prusament"},
  {47560, "Wanhao"},
  {47930, "eSun"},
  {48261, "Gsun3D"},
  {48804, "R3D"},
  {49784, "GIANTARN"},
  {50311, "G3D Pro"},
  {50604, "Polymaker"},
  {51139, "FusRock"},
  {51443, "BASF"},
  {51857, "Sunlu"},
  {52222, "ColorFabb"},
  {52467, "Geeetech"},
  {52757, "Yumi"},
  {53043, "FormFutura"},
  {53640, "Magigoo"},
  {53856, "Lattice Medical"},
  {54112, "Kexcelled"},
  {55229, "Filament PM"},
  {55763, "Nanovia"},
  {55869, "Biqu"},
  {56780, "Fiberon"},
  {56789, "Coex 3D"},
  {57209, "FrancoFil"},
  {57632, "ELEGOO"},
  {58231, "IC3D"},
  {58410, "AzureFilm"},
  {58972, "Tinmorry"},
  {59597, "Filamentive"},
  {60882, "Recreus"},
  {62436, "Ambrosia"},
  {63024, "Multicomp Pro"},
  {63340, "Flashforge"},
  {65535, "Generic"},
};
static const int ttBrandsCount = 122;

static const TigerTagEntry ttAspects[] = {
  {0, "-"},
  {21, "Clear"},
  {24, "Tricolor"},
  {64, "Glitter"},
  {67, "Translucent"},
  {91, "Glow in the Dark"},
  {92, "Silk"},
  {97, "Lithophane"},
  {104, "Basic"},
  {123, "Wood"},
  {126, "Pearl"},
  {129, "Gloss"},
  {134, "Satin"},
  {145, "Rainbow"},
  {168, "Thermoreactif"},
  {173, "Stone"},
  {216, "Neon"},
  {220, "Pastel"},
  {226, "Metal"},
  {232, "Marble"},
  {238, "Carbon"},
  {247, "Matt"},
  {252, "Bicolor"},
  {255, "None"},
};
static const int ttAspectsCount = 24;

struct TigerTagDiam { uint8_t id; float mm; };
static const TigerTagDiam ttDiameters[] = {
  {56, 1.75f}, {221, 2.85f}
};
static const int ttDiametersCount = 2;

// ── Lookup helpers ─────────────────────────────────────────────
static const char* ttLookupMaterialLabel(uint16_t id) {
  for (int i = 0; i < ttMaterialsCount; i++)
    if ((uint32_t)ttMaterials[i].id == id) return ttMaterials[i].label;
  return "?";
}
static const char* ttLookupMaterialType(uint16_t id) {
  for (int i = 0; i < ttMaterialsCount; i++)
    if ((uint32_t)ttMaterials[i].id == id) return ttMaterials[i].matType;
  return "";
}
static const char* ttLookupFilledType(uint16_t id) {
  for (int i = 0; i < ttMaterialsCount; i++)
    if ((uint32_t)ttMaterials[i].id == id) return ttMaterials[i].filledType;
  return "";
}
static const char* ttLookupBrand(uint16_t id) {
  for (int i = 0; i < ttBrandsCount; i++)
    if ((uint32_t)ttBrands[i].id == id) return ttBrands[i].label;
  return "?";
}
static const char* ttLookupAspect(uint8_t id) {
  for (int i = 0; i < ttAspectsCount; i++)
    if ((uint8_t)ttAspects[i].id == id) return ttAspects[i].label;
  return "";
}
static float ttLookupDiameter(uint8_t id) {
  for (int i = 0; i < ttDiametersCount; i++)
    if (ttDiameters[i].id == id) return ttDiameters[i].mm;
  return 1.75f;
}

// ── NTAG raw-page reader constants ────────────────────────────
// (NTAG constants in config.h — NTAG_MAX_PAGES, TT_USER_*)


static int ndefParseRecords(const uint8_t* buf, int nPages,
                             NdefRecord* recs, int maxRecs) {
  if (nPages < 8) return 0;
  if (buf[12] != 0xE1) return 0;  // CC magic byte at page 3

  const int bufLen = nPages * 4;
  int pos = 16;  // user data starts at page 4
  int nRecs = 0;

  while (pos < bufLen) {
    uint8_t tlvType = buf[pos++];
    if (tlvType == 0xFE || pos >= bufLen) break;
    if (tlvType == 0x00) continue;

    uint8_t lb = buf[pos++];
    int tlvLen;
    if (lb == 0xFF) {
      if (pos + 2 > bufLen) break;
      tlvLen = ((int)buf[pos] << 8) | buf[pos + 1];
      pos += 2;
    } else {
      tlvLen = lb;
    }
    if (tlvType != 0x03) { pos += tlvLen; continue; }

    int ndefEnd = pos + tlvLen;
    if (ndefEnd > bufLen) ndefEnd = bufLen;

    while (pos < ndefEnd && nRecs < maxRecs) {
      uint8_t hdr = buf[pos++];
      bool sr  = (hdr >> 4) & 1;
      bool il  = (hdr >> 3) & 1;
      uint8_t tnf = hdr & 0x07;

      if (pos >= ndefEnd) break;
      uint8_t typeLen = buf[pos++];

      int payloadLen = 0;
      if (sr) {
        if (pos >= ndefEnd) break;
        payloadLen = buf[pos++];
      } else {
        if (pos + 4 > ndefEnd) break;
        payloadLen = ((int)buf[pos] << 24) | ((int)buf[pos+1] << 16)
                   | ((int)buf[pos+2] << 8)  |  buf[pos+3];
        pos += 4;
      }
      uint8_t idLen = 0;
      if (il) { if (pos >= ndefEnd) break; idLen = buf[pos++]; }

      NdefRecord& rec = recs[nRecs];
      rec.tnf = tnf;
      int tl = min((int)typeLen, (int)sizeof(rec.type) - 1);
      memcpy(rec.type, buf + pos, tl);
      rec.type[tl] = '\0';
      pos += typeLen;
      pos += idLen;

      rec.payloadLen = min(payloadLen, NDEF_MAX_PAYLOAD);
      if (pos + rec.payloadLen <= ndefEnd)
        memcpy(rec.payload, buf + pos, rec.payloadLen);
      rec.payload[rec.payloadLen] = '\0';
      pos += payloadLen;
      nRecs++;
    }
    break;
  }
  return nRecs;
}

// ── TigerTag binary format parser ─────────────────────────────
// Offsets relative to user-data start (page 4, buf[16])
static bool tryParseTigerTag(const uint8_t* buf, int nPages, TagInfo* t) {
  if (nPages < TT_MIN_READ_PAGES) return false;
  const uint8_t* ud = buf + TT_USER_BYTE_START;

  uint32_t tagId = ((uint32_t)ud[0] << 24) | ((uint32_t)ud[1] << 16)
                 | ((uint32_t)ud[2] <<  8) |            ud[3];
  if (tagId != 0x5BF59264UL && tagId != 0xBC0FCB97UL) return false;

  uint16_t matId   = ((uint16_t)ud[ 8] << 8) | ud[ 9];
  uint8_t  asp1Id  = ud[10];
  uint8_t  asp2Id  = ud[11];
  uint8_t  diamId  = ud[13];
  uint16_t brandId = ((uint16_t)ud[14] << 8) | ud[15];
  uint8_t  r = ud[16], g = ud[17], b = ud[18];
  uint32_t wRaw = ((uint32_t)ud[20] << 16) | ((uint32_t)ud[21] << 8) | ud[22];
  uint8_t  unitId  = ud[23];
  uint16_t tMin    = ((uint16_t)ud[24] << 8) | ud[25];
  uint16_t tMax    = ((uint16_t)ud[26] << 8) | ud[27];
  uint8_t  dryTmp  = ud[28];
  uint8_t  dryTm   = ud[29];
  uint8_t  bTMin   = ud[30];
  uint8_t  bTMax   = ud[31];

  const char* matType    = ttLookupMaterialType(matId);
  const char* filledType = ttLookupFilledType(matId);
  const char* matLabel   = ttLookupMaterialLabel(matId);
  const char* brandName  = ttLookupBrand(brandId);
  const char* asp1Label  = ttLookupAspect(asp1Id);
  const char* asp2Label  = ttLookupAspect(asp2Id);
  float       diamMm     = ttLookupDiameter(diamId);

  if (filledType[0] && matType[0])
    snprintf(t->filamentType, sizeof(t->filamentType), "%s-%s", matType, filledType);
  else if (matType[0])
    snprintf(t->filamentType, sizeof(t->filamentType), "%s", matType);
  else
    snprintf(t->filamentType, sizeof(t->filamentType), "%s", matLabel);

  t->detailedType[0] = '\0';
  bool a1 = asp1Id != 0 && asp1Id != 255 && asp1Label[0]
         && strcmp(asp1Label, "-") != 0 && strcmp(asp1Label, "None") != 0;
  bool a2 = asp2Id != 0 && asp2Id != 255 && asp2Label[0]
         && strcmp(asp2Label, "-") != 0 && strcmp(asp2Label, "None") != 0;
  if (a1 && a2)  snprintf(t->detailedType, sizeof(t->detailedType), "%s / %s", asp1Label, asp2Label);
  else if (a1)   snprintf(t->detailedType, sizeof(t->detailedType), "%s", asp1Label);
  else if (a2)   snprintf(t->detailedType, sizeof(t->detailedType), "%s", asp2Label);

  snprintf(t->variantId,  sizeof(t->variantId),  "%s", brandName);
  snprintf(t->materialId, sizeof(t->materialId), "%s", matLabel);

  t->colorR = r; t->colorG = g; t->colorB = b;
  t->diameter = diamMm;

  float wG = (float)wRaw;
  if (unitId == 35)      wG *= 1000.0f;
  else if (unitId == 10) wG /= 1000.0f;
  t->spoolWeight = (uint16_t)wG;

  t->minNozzleTemp = tMin;
  t->maxNozzleTemp = tMax;
  t->bedTemp       = ((uint16_t)bTMin + bTMax) / 2;
  t->dryTemp       = dryTmp;
  t->dryTime       = dryTm;
  t->filamentLength = 0;
  t->tagSource = TAG_SRC_TIGERTAG;
  return true;
}

// ── OpenSpool NDEF JSON parser ─────────────────────────────────
static bool tryParseOpenSpool(const NdefRecord* recs, int nRecs, TagInfo* t) {
  for (int i = 0; i < nRecs; i++) {
    const NdefRecord& rec = recs[i];
    if (rec.tnf != 0x02 || strcmp(rec.type, "application/json") != 0) continue;
    if (rec.payloadLen <= 0) continue;

    DynamicJsonDocument jdoc(512);
    if (deserializeJson(jdoc, (const char*)rec.payload, rec.payloadLen) != DeserializationError::Ok)
      continue;
    if (strcmp(jdoc["protocol"] | "", "openspool") != 0) continue;

    const char* brand   = jdoc["brand"]     | "Generic";
    const char* type    = jdoc["type"]      | "PLA";
    const char* sub     = jdoc["subtype"]   | "";
    const char* clrHex  = jdoc["color_hex"] | "FFFFFF";
    float  diam   = jdoc["diameter"]  | 1.75f;
    int    weight = jdoc["weight"]    | 1000;
    int    tMin   = jdoc["min_temp"]  | 0;
    int    tMax   = jdoc["max_temp"]  | 0;

    const char* h = clrHex;  if (*h == '#') h++;
    uint32_t cv = strtoul(h, nullptr, 16);

    snprintf(t->filamentType, sizeof(t->filamentType), "%s", type);
    for (char* p = t->filamentType; *p; p++) *p = toupper((unsigned char)*p);
    snprintf(t->detailedType, sizeof(t->detailedType), "%s", sub);
    snprintf(t->variantId,    sizeof(t->variantId),    "%s", brand);
    t->materialId[0] = '\0';

    t->colorR = (cv >> 16) & 0xFF;
    t->colorG = (cv >>  8) & 0xFF;
    t->colorB =  cv        & 0xFF;

    t->diameter      = diam;
    t->spoolWeight   = (uint16_t)weight;
    t->minNozzleTemp = (uint16_t)tMin;
    t->maxNozzleTemp = (uint16_t)tMax;
    t->bedTemp = t->dryTemp = t->dryTime = t->filamentLength = 0;
    t->tagSource = TAG_SRC_OPENSPOOL;
    return true;
  }
  return false;
}

// ── SpoolEase NDEF URI detector ────────────────────────────────
static bool tryParseSpoolEase(const NdefRecord* recs, int nRecs, TagInfo* t) {
  static const char* const uriPfx[] = {
    "", "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:"
  };
  for (int i = 0; i < nRecs; i++) {
    const NdefRecord& rec = recs[i];
    if (rec.tnf != 0x01 || rec.type[0] != 'U' || rec.type[1] != '\0') continue;
    if (rec.payloadLen < 2) continue;

    uint8_t pfxCode = rec.payload[0];
    const char* pfx = (pfxCode < 7) ? uriPfx[pfxCode] : "";
    char url[sizeof(t->tagUrl)];
    snprintf(url, sizeof(url), "%s%.*s",
             pfx, rec.payloadLen - 1, (const char*)(rec.payload + 1));

    if (!strstr(url, "spoolease.io") && !strstr(url, "SpoolEase")) continue;

    snprintf(t->filamentType, sizeof(t->filamentType), "SpoolEase");
    t->detailedType[0] = '\0';
    t->variantId[0]    = '\0';
    t->materialId[0]   = '\0';
    strncpy(t->tagUrl, url, sizeof(t->tagUrl) - 1);
    t->tagUrl[sizeof(t->tagUrl) - 1] = '\0';
    t->tagSource = TAG_SRC_SPOOLEASE;
    return true;
  }
  return false;
}

// ── OpenTag3D NDEF parser ─────────────────────────────────────
static bool tryParseOpenTag3D(const NdefRecord* recs, int nRecs, TagInfo* t) {
  for (int i = 0; i < nRecs; i++) {
    const NdefRecord& rec = recs[i];
    if (rec.tnf != 0x02 || strcmp(rec.type, "application/opentag3d") != 0) continue;
    if (rec.payloadLen < 0x66) continue;

    const uint8_t* p = rec.payload;
    uint16_t ver = ((uint16_t)p[0] << 8) | p[1];
    (void)ver;  // version

    char baseMat[6] = {0}; memcpy(baseMat, p + 0x02, 5);
    char mods[6] = {0};   memcpy(mods,   p + 0x07, 5);
    char brand[17] = {0}; memcpy(brand,  p + 0x1B, 16);
    char clrName[33]={0}; memcpy(clrName,p + 0x2B, 32);
    uint8_t r = p[0x4B], g = p[0x4C], b = p[0x4D];
    uint16_t diam = ((uint16_t)p[0x5C] << 8) | p[0x5D];
    uint16_t wg   = ((uint16_t)p[0x5E] << 8) | p[0x5F];
    uint16_t pt   = (uint16_t)p[0x60] * 5;
    uint16_t bt   = (uint16_t)p[0x61] * 5;

    trimStr(baseMat, 5); trimStr(mods, 5); trimStr(brand, 16); trimStr(clrName, 32);

    if (mods[0])
      snprintf(t->filamentType,  sizeof(t->filamentType),  "%s-%s", baseMat, mods);
    else
      snprintf(t->filamentType,  sizeof(t->filamentType),  "%s", baseMat);
    t->detailedType[0] = '\0';
    snprintf(t->variantId,       sizeof(t->variantId),       "%s", clrName[0] ? clrName : brand);
    snprintf(t->materialId,      sizeof(t->materialId),      "%s", brand);

    t->colorR = r; t->colorG = g; t->colorB = b;
    t->diameter = diam / 1000.0f;
    t->spoolWeight = wg;
    t->minNozzleTemp = pt; t->maxNozzleTemp = pt + 10;
    t->bedTemp = bt;
    t->dryTemp = t->dryTime = t->filamentLength = 0;
    t->tagSource = TAG_SRC_OPENTAG3D;
    return true;
  }
  return false;
}

// ── Unified NTAG reader (card already selected by caller) ──────
static bool rfidReadNTAGTag(TagInfo* t) {
  static uint8_t ntagBuf[NTAG_MAX_PAGES * 4];
  memset(ntagBuf, 0, sizeof(ntagBuf));
  int nPages = 0;

  for (int page = 0; page < NTAG_MAX_PAGES; page += 4) {
    uint8_t rbuf[18]; uint8_t sz = 18;
    if (rfid.MIFARE_Read(page, rbuf, &sz) != MFRC522::STATUS_OK) break;
    int n = min(4, NTAG_MAX_PAGES - page);
    memcpy(ntagBuf + page * 4, rbuf, n * 4);
    nPages = page + n;
  }
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (nPages < TT_MIN_READ_PAGES) return false;

  if (tryParseTigerTag(ntagBuf, nPages, t)) {
    // Save page data for later writing
    ntagWritePages = nPages;
    memcpy(ntagWriteBuf, ntagBuf, nPages * 4);
    t->valid = true; return true;
  }

  static NdefRecord ndefRecs[NDEF_MAX_RECORDS];
  int nRecs = ndefParseRecords(ntagBuf, nPages, ndefRecs, NDEF_MAX_RECORDS);
  if (nRecs > 0) {
    if (tryParseOpenSpool(ndefRecs, nRecs, t)) { t->valid = true; return true; }
    if (tryParseOpenTag3D(ndefRecs, nRecs, t)) { t->valid = true; return true; }
    if (tryParseSpoolEase(ndefRecs, nRecs, t)) { t->valid = true; return true; }
  }

  snprintf(t->filamentType, sizeof(t->filamentType), "Unknown NTAG");
  t->tagSource = TAG_SRC_UNKNOWN_NTAG;
  t->valid = true;
  return true;
}

// ── Dispatch: detect card type, route to correct reader ───────
static bool rfidDetectAndReadTag(TagInfo* t) {
  memset(t, 0, sizeof(TagInfo));
  t->valid = false;

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
    return false;

  MFRC522::PICC_Type pt = rfid.PICC_GetType(rfid.uid.sak);

  if (pt == MFRC522::PICC_TYPE_MIFARE_UL) {
    int ul = min((int)rfid.uid.size, 4);
    memcpy(t->uid, rfid.uid.uidByte, ul);
    t->tagSource = TAG_SRC_UNKNOWN_NTAG;
    return rfidReadNTAGTag(t);
  }

  // Mifare Classic → Bambu Lab path.
  // Halt the current card and power-cycle the antenna so the card returns to
  // IDLE state.  rfidReadBambuTag() then does its own PICC_IsNewCardPresent +
  // PICC_ReadCardSerial — we must NOT call rfidReSelect() here because that
  // would fully re-select the card and leave it ACTIVE, causing the second
  // PICC_IsNewCardPresent() inside rfidReadBambuTag() to fail.
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  rfid.PCD_AntennaOff();
  delay(30);   // card capacitor drains → IDLE state
  rfid.PCD_AntennaOn();
  delay(20);   // RF field stabilises, card powers up
  bool ok = rfidReadBambuTag(t);
  if (ok) t->tagSource = TAG_SRC_BAMBU;
  return ok;
}

// ── NTAG (Ultralight) page writer for TigerTag / OpenSpool ──────
// Writes pages 3..nPages-1 using MIFARE_Ultralight_Write,
static bool rfidWriteNTAGPages(const uint8_t* pageData, int nPages) {
  for (int page = 3; page < nPages; page++) {
    byte buf[4];
    memcpy(buf, pageData + page * 4, 4);
    if (rfid.MIFARE_Ultralight_Write(page, buf, 4) != MFRC522::STATUS_OK) {
      DBGF("[NTAG] write page %d FAIL\n", page);
      return false;
    }
  }
  return true;
}

// ── TigerTag encoder: build binary page buffer ──────────────────
// Fills ntagWriteBuf[0..59] (pages 4-18) and sets ntagWritePages=19.
// All multi-byte values are big-endian as per TigerTag spec.
static void buildTigerTag(uint16_t matId, uint16_t brandId,
                           uint8_t r, uint8_t g, uint8_t b,
                           uint16_t weightG, float diamMm,
                           uint16_t tMin, uint16_t tMax) {
  memset(ntagWriteBuf, 0, NTAG_MAX_PAGES * 4);
  int base = TT_USER_BYTE_START;  // page 4 → byte 16
  // Tag ID (4 bytes big-endian at user-data offset 0)
  uint32_t tagId = 0x5BF59264UL;
  ntagWriteBuf[base +  0] = (tagId >> 24) & 0xFF;
  ntagWriteBuf[base +  1] = (tagId >> 16) & 0xFF;
  ntagWriteBuf[base +  2] = (tagId >>  8) & 0xFF;
  ntagWriteBuf[base +  3] =  tagId        & 0xFF;
  // Material ID  (offset 8, uint16 big-endian)
  ntagWriteBuf[base +  8] = (matId >> 8) & 0xFF;
  ntagWriteBuf[base +  9] =  matId       & 0xFF;
  // Diameter (offset 13)
  ntagWriteBuf[base + 13] = (diamMm >= 2.85f) ? 221 : 56;
  // Brand ID (offset 14, uint16 big-endian)
  ntagWriteBuf[base + 14] = (brandId >> 8) & 0xFF;
  ntagWriteBuf[base + 15] =  brandId       & 0xFF;
  // Color (offset 16-18, RGB)
  ntagWriteBuf[base + 16] = r;
  ntagWriteBuf[base + 17] = g;
  ntagWriteBuf[base + 18] = b;
  // Weight (offset 20-22, 3 bytes big-endian, unit grams)
  ntagWriteBuf[base + 20] = (weightG >> 16) & 0xFF;
  ntagWriteBuf[base + 21] = (weightG >>  8) & 0xFF;
  ntagWriteBuf[base + 22] =  weightG        & 0xFF;
  ntagWriteBuf[base + 23] = 35;  // unit: grams (35)
  // Temperatures (offset 24-31)
  ntagWriteBuf[base + 24] = (tMin >> 8) & 0xFF;
  ntagWriteBuf[base + 25] =  tMin       & 0xFF;
  ntagWriteBuf[base + 26] = (tMax >> 8) & 0xFF;
  ntagWriteBuf[base + 27] =  tMax       & 0xFF;
  ntagWriteBuf[base + 28] = 55;
  ntagWriteBuf[base + 29] = 6;
  ntagWriteBuf[base + 30] = 55;
  ntagWriteBuf[base + 31] = 60;
  ntagWritePages = 19;
}

// ── OpenSpool NDEF encoder ─────────────────────────────────────
static void buildOpenSpool(const char* type, const char* brand,
                            uint8_t r, uint8_t g, uint8_t b,
                            uint16_t weightG, float diamMm,
                            uint16_t tMin, uint16_t tMax) {
  char hex[8];
  snprintf(hex, sizeof(hex), "%02X%02X%02X", r, g, b);
  char json[256];
  snprintf(json, sizeof(json),
    "{\"protocol\":\"openspool\",\"brand\":\"%s\",\"type\":\"%s\","
    "\"subtype\":\"\",\"color_hex\":\"%s\",\"diameter\":%.2f,"
    "\"weight\":%d,\"min_temp\":%d,\"max_temp\":%d}",
    brand, type, hex, diamMm, weightG, tMin, tMax);

  memset(ntagWriteBuf, 0, 200);
  int base = TT_USER_BYTE_START;
  // Page 3: Capability Container
  ntagWriteBuf[12] = 0xE1;  // NDEF magic
  ntagWriteBuf[13] = 0x10;  // version 1.0
  ntagWriteBuf[14] = 0x06;  // memory size / 8
  ntagWriteBuf[15] = 0x00;

  // Page 4+: TLV
  int jsonLen = strlen(json);
  int ndefLen = 1 + 1 + 1 + 19 + jsonLen;
  ntagWriteBuf[base + 0] = 0x03;
  if (ndefLen < 255) {
    ntagWriteBuf[base + 1] = (uint8_t)ndefLen;
    ntagWriteBuf[base + 2] = 0xD2;
    ntagWriteBuf[base + 3] = 19;
    ntagWriteBuf[base + 4] = (uint8_t)jsonLen;
    memcpy(ntagWriteBuf + base + 5, "application/json", 19);
    memcpy(ntagWriteBuf + base + 24, json, jsonLen);
    // TLV terminator
    int end = base + 2 + ndefLen;
    ntagWriteBuf[end] = 0xFE;
    ntagWritePages = (end + 4) / 4;  // round up to pages
  }
}

// ── OpenTag3D encoder ─────────────────────────────────────────
static void buildOpenTag3D(const char* matType, const char* brand,
                            uint8_t r, uint8_t g, uint8_t b,
                            uint16_t weightG, float diamMm,
                            uint16_t tMin, uint16_t tMax) {
  memset(ntagWriteBuf, 0, 200);
  // Page 3: Capability Container
  ntagWriteBuf[12] = 0xE1; ntagWriteBuf[13] = 0x10;
  ntagWriteBuf[14] = 0x06; ntagWriteBuf[15] = 0x00;

  // Build binary payload in a temp buffer
  uint8_t payload[112];
  memset(payload, 0, sizeof(payload));
  payload[0x00] = 0x03; payload[0x01] = 0xE8;

  // Split material into base + modifier (e.g. "PETG-GF" → base="PETG", mod="GF")
  char base[6] = {0}, mod[6] = {0};
  const char* dash = strrchr(matType, '-');
  if (dash) {
    int blen = min((int)(dash - matType), 5);
    memcpy(base, matType, blen);
    strncpy(mod, dash + 1, 5);
  } else {
    strncpy(base, matType, 5);
  }
  strncpy((char*)(payload + 0x02), base, 5);
  strncpy((char*)(payload + 0x07), mod, 5);
  strncpy((char*)(payload + 0x1B), brand, 16);
  payload[0x4B] = r; payload[0x4C] = g; payload[0x4D] = b; payload[0x4E] = 0xFF;
  uint16_t d = (uint16_t)(diamMm * 1000.0f);
  payload[0x5C] = (d >> 8) & 0xFF; payload[0x5D] = d & 0xFF;
  payload[0x5E] = (weightG >> 8) & 0xFF; payload[0x5F] = weightG & 0xFF;
  payload[0x60] = (uint8_t)(tMin / 5);
  payload[0x61] = (uint8_t)((tMax > 0 ? 55 : 0) / 5);
  payload[0x62] = 0x04; payload[0x63] = 0xD8;
  int payLen = 0x66;

  // Write TLV + NDEF at page 4 (byte 16)
  int ofs = TT_USER_BYTE_START;
  int ndefLen = 1 + 1 + 1 + 21 + payLen;
  ntagWriteBuf[ofs + 0] = 0x03;
  ntagWriteBuf[ofs + 1] = (uint8_t)ndefLen;
  int p = ofs + 2;
  ntagWriteBuf[p++] = 0xD2;       // NDEF SR record
  ntagWriteBuf[p++] = 21;         // type length
  ntagWriteBuf[p++] = (uint8_t)payLen;
  memcpy(ntagWriteBuf + p, "application/opentag3d", 21); p += 21;
  memcpy(ntagWriteBuf + p, payload, payLen); p += payLen;
  ntagWriteBuf[p] = 0xFE;  // terminator
  ntagWritePages = (p + 4) / 4;
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
  lcd.fillScreen(COL_BG);
  drawStatusBar(); drawSubHeader(header);
  int startY = 118;
  if (info && info[0]) {
    lcd.setTextColor(COL_SUBTEXT, COL_BG); lcd.setTextSize(3);
    lcd.setCursor(12, 130); lcd.print(info);
    startY = 164;
  }
  static const char* opts[3] = { "Skip", "Seal", "Unlock" };
  const int bw = 350, bh = 80, gap = 16;
  lcd.setTextSize(3);
  for (int i = 0; i < 3; i++) {
    int x = (LCD_WIDTH - bw) / 2;
    int y = startY + i * (bh + gap);
    uint16_t btnColor = (i == 1) ? COL_ACC : (i == 2) ? COL_RED : COL_CARD;
    drawBtn(x, y, bw, bh, btnColor, opts[i]);
  }
  lcd.setCursor(LCD_WIDTH / 2 - 70, startY + 3 * (bh + gap) + 10);
  lcd.setTextColor(COL_SUBTEXT, COL_BG);
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
  lcd.fillScreen(COL_BG);
  drawStatusBar(); drawSubHeader(header);
  int startY = 114;
  if (info && info[0]) {
    lcd.setTextColor(COL_SUBTEXT, COL_BG); lcd.setTextSize(3);
    lcd.setCursor(12, 130); lcd.print(info);
    startY = 162;
  }
  static const char* OPTS[] = { "Skip", "Repair Tag", "Lock Block 0", "Unlock Block 0" };
  const int bw = 350, bh = 60, gap = 6;
  lcd.setTextSize(3);
  for (int i = 0; i < 4; i++) {
    int x = (LCD_WIDTH - bw) / 2;
    int y = startY + i * (bh + gap);
    uint16_t btnColor = (i == 1) ? COL_ACC : (i == 3) ? COL_ACC : COL_CARD;
    drawBtn(x, y, bw, bh, btnColor, OPTS[i]);
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
  lcd.setTextColor(COL_TEXT, bg);
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
  uint16_t green = COL_ACC;
  uint16_t dark  = COL_GREY;

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
  lcd.fillRect(0, 0, LCD_WIDTH, 64, COL_SIDEBAR);
  if (logoBuffer) lcd.pushImage(4, 0, LOGO_W, LOGO_H, logoBuffer);
  lcd.setTextColor(COL_TEXT, COL_SIDEBAR); lcd.setTextSize(5);
  lcd.setTextDatum(MC_DATUM);
  lcd.drawString("BambuTagger-Touch", LCD_WIDTH / 2, 32);
  drawWiFiIcon(LCD_WIDTH - 48, 32);
}

#define SUBHEADER_Y 64
#define SUBHEADER_H 44

static void drawSubHeader(const char* title) {
  lcd.fillRect(0, SUBHEADER_Y, LCD_WIDTH, SUBHEADER_H, COL_CARD);
  lcd.setTextColor(COL_TEXT, COL_CARD);
  lcd.setTextSize(3);
  lcd.setTextDatum(ML_DATUM);
  lcd.drawString(title, 10, SUBHEADER_Y + SUBHEADER_H / 2);
}
#define BACK_X ((LCD_WIDTH - 200) / 2)
#define BACK_Y (LCD_HEIGHT - 24 - 56 - 5)
#define BACK_W 200
#define BACK_H 56
static void drawBackBtn() { lcd.setTextSize(2); drawBtn(BACK_X, BACK_Y, BACK_W, BACK_H, COL_SLATE, "Back"); }
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
  lcd.fillRoundRect(SB_X, sy, SB_W, sh, 3, COL_CARD);
  int thumbH = max(16, sh * visRows / totalRows);
  int thumbY = sy + (sh - thumbH) * scrollPos / (totalRows - visRows);
  lcd.fillRoundRect(SB_X, thumbY, SB_W, thumbH, 3, COL_SLATE);
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
  lcd.fillRect(0, FOOTER_Y, LCD_WIDTH, FOOTER_H, COL_SIDEBAR);
  lcd.setTextColor(COL_SUBTEXT, COL_SIDEBAR); lcd.setTextSize(2);
  lcd.setTextDatum(MC_DATUM);
  lcd.drawString("(c) 2026 BambuTagger", LCD_WIDTH / 2, FOOTER_Y + FOOTER_H / 2);
  lcd.setTextDatum(TL_DATUM);
  lcd.setCursor(LCD_WIDTH - 75, FOOTER_Y + 4);
  lcd.print("v" FIRMWARE_VERSION);
}

void showStatus(const char* msg) {
  lcd.fillScreen(COL_BG);
  drawStatusBar();
  // Extract first line as subheader
  const char* nl = strchr(msg, '\n');
  int titleLen = nl ? min((int)(nl - msg), 31) : (int)strlen(msg);
  char title[32];
  if (titleLen) { strncpy(title, msg, titleLen); title[titleLen] = '\0'; }
  else title[0] = '\0';
  if (title[0]) drawSubHeader(title);

  // Remaining lines
  lcd.setTextColor(COL_TEXT, COL_BG);
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
  lcd.fillScreen(COL_BG);
  drawStatusBar(); drawSubHeader(l1);
  lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(3);
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
  lcd.fillScreen(COL_BG);
  drawStatusBar(); drawSubHeader("Menu");
  lcd.setTextSize(3);
  for (int i = 0; i < MENU_COUNT; i++) {
    int col = i / MENU_ROWS;
    int row = i % MENU_ROWS;
    int x = MENU_X0 + col * (BTN_W + BTN_GAP_X);
    int y = MENU_Y0 + row * (BTN_H + BTN_GAP_Y);
    uint16_t bg = COL_CARD;
    drawBtn(x, y, BTN_W, BTN_H, bg, MENU_ITEMS[i]);
  }
  drawFooter(); lcd.display();
}

// ──────────────────────────────────────────────────────────────
//  Draw tag info
// ──────────────────────────────────────────────────────────────

// ──────────────────────────────────────────────────────────────
//  Draw dump-file selection list
// ──────────────────────────────────────────────────────────────

// ──────────────────────────────────────────────────────────────
//  GitHub OLED browser state
// ──────────────────────────────────────────────────────────────
#define GH_MAX_ENTRIES 48

// ──────────────────────────────────────────────────────────────
//  BambuMan catalog OLED browser state
// ──────────────────────────────────────────────────────────────
#define BM_MAX_ENTRIES 64

// ── BambuMan 3-level navigation cache (Mat / Type / Color) ────
// Levels 0–2 are served from RAM; level 3 (UIDs) always streams the file.
#define BM_CACHE_L0  24    // max distinct materials
#define BM_CACHE_L1  96    // max distinct mat+type combos
#define BM_CACHE_L2  128   // max distinct mat+type+color combos


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
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  for (int i = 0; i < 24 && WiFi.status() != WL_CONNECTED; i++)
    delay(500);
  return WiFi.status() == WL_CONNECTED;
}

void wifiStartAP() {
  DBGLN("[WiFi]  Starting AP mode...");
  WiFi.mode(WIFI_AP);
  WiFi.softAPsetHostname(HOSTNAME);
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
<title>BambuTagger-Touch</title>
<link rel="icon" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgAgMAAAAOFJJnAAABhWlDQ1BJQ0MgcHJvZmlsZQAAKJF9kb9Lw0AcxV9bS6VUHawg4pChOrWLijiWKhbBQmkrtOpgcukvaNKQpLg4Cq4FB38sVh1cnHV1cBUEwR8g/gHipOgiJX4vKbSI8eC4D+/uPe7eAd5WjSlGXxxQVFPPJBNCvrAqBF7hRxAjGERUZIaWyi7m4Dq+7uHh612MZ7mf+3MMyEWDAR6BOM403STeIJ7dNDXO+8RhVhFl4nPiqE4XJH7kuuTwG+eyzV6eGdZzmXniMLFQ7mGph1lFV4hniCOyolK+N++wzHmLs1JrsM49+QtDRXUly3Wa40hiCSmkIUBCA1XUYCJGq0qKgQztJ1z8Y7Y/TS6JXFUwciygDgWi7Qf/g9/dGqXpKScplAD8L5b1MQEEdoF207K+jy2rfQL4noErteuvt4C5T9KbXS1yBAxtAxfXXU3aAy53gNEnTdRFW/LR9JZKwPsZfVMBGL4FgmtOb519nD4AOepq+QY4OAQmy5S97vLu/t7e/j3T6e8HrYRyvp7c8c0AAAAJUExURXIA83m/boC9efRkY8YAAAABdFJOUwBA5thmAAAAAWJLR0QAiAUdSAAAAL1JREFUGNNNkLEKg0AMhv8GHO52H0FR36SbCJHD6XASn+Lazb1XHG8R1Kds7kqLgZAvGZL/D3CJbXCp1sxTAs/cx5HmvWErUJhvYgsA9QJv6AAvzUqeXe5Au5qPNrMyL4GXslCuAppbK4B6AhkUDr6PUDpYBQiUgZw2Bs02KJsn4KVjgWLh+8idQbawVTwaqGeERwttfZv1coKMXrMgR2lFVcX9f2GIm5PUwpBL4omPOdlJBpNT9bOM87y+5AM/WTesHvLO9wAAAABJRU5ErkJggg==" type="image/svg+xml" />
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:#0d1117;color:#c9d1d9;min-height:100vh}
a{text-decoration:none;color:#c9d1d9}
a:hover{text-decoration:none;color:#efefef}
header{background:#161b22;border-bottom:1px solid #30363d;padding:12px 24px}
header .logo{display:flex;align-items:center;gap:10px}
header .logo img{width:28px;height:28px;flex-shrink:0;border-radius:4px}
header h1{font-size:18px;color:#58a6ff}
nav{background:#161b22;border-bottom:1px solid #30363d;display:flex;gap:0}
nav a{padding:10px 20px;color:#8b949e;text-decoration:none;font-size:14px;border-bottom:2px solid transparent;cursor:pointer}
nav a:hover{color:#c9d1d9}
nav a.active{color:#58a6ff;border-bottom-color:#58a6ff}
footer{position:fixed;bottom:0;left:0;right:0;text-align:center;padding:6px;font-size:10px;color:#484f58;background:#0d1117;border-top:1px solid #30363d}
footer a{color:#484f58;text-decoration:none}
footer a:hover{color:#c9d1d9}
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
.warn{background:#3d2a00;color:#d29922;border:1px solid #bb8009}
.pending{background:#1c1335;color:#a371f7;border:1px solid #8256d0}
.neutral{background:#161b22;color:#8b949e;border:1px solid #30363d}
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
<header><div class="logo"><img src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgAgMAAAAOFJJnAAABhWlDQ1BJQ0MgcHJvZmlsZQAAKJF9kb9Lw0AcxV9bS6VUHawg4pChOrWLijiWKhbBQmkrtOpgcukvaNKQpLg4Cq4FB38sVh1cnHV1cBUEwR8g/gHipOgiJX4vKbSI8eC4D+/uPe7eAd5WjSlGXxxQVFPPJBNCvrAqBF7hRxAjGERUZIaWyi7m4Dq+7uHh612MZ7mf+3MMyEWDAR6BOM403STeIJ7dNDXO+8RhVhFl4nPiqE4XJH7kuuTwG+eyzV6eGdZzmXniMLFQ7mGph1lFV4hniCOyolK+N++wzHmLs1JrsM49+QtDRXUly3Wa40hiCSmkIUBCA1XUYCJGq0qKgQztJ1z8Y7Y/TS6JXFUwciygDgWi7Qf/g9/dGqXpKScplAD8L5b1MQEEdoF207K+jy2rfQL4noErteuvt4C5T9KbXS1yBAxtAxfXXU3aAy53gNEnTdRFW/LR9JZKwPsZfVMBGL4FgmtOb519nD4AOepq+QY4OAQmy5S97vLu/t7e/j3T6e8HrYRyvp7c8c0AAAAJUExURXIA83m/boC9efRkY8YAAAABdFJOUwBA5thmAAAAAWJLR0QAiAUdSAAAAL1JREFUGNNNkLEKg0AMhv8GHO52H0FR36SbCJHD6XASn+Lazb1XHG8R1Kds7kqLgZAvGZL/D3CJbXCp1sxTAs/cx5HmvWErUJhvYgsA9QJv6AAvzUqeXe5Au5qPNrMyL4GXslCuAppbK4B6AhkUDr6PUDpYBQiUgZw2Bs02KJsn4KVjgWLh+8idQbawVTwaqGeERwttfZv1coKMXrMgR2lFVcX9f2GIm5PUwpBL4omPOdlJBpNT9bOM87y+5AM/WTesHvLO9wAAAABJRU5ErkJggg==" alt="logo"><h1>BambuTagger-Touch</h1></div></header>
<nav>
<a href="#" class="active"  id="tab-local-btn"  onclick="switchTab('local')">Local Library</a>
<a href="#"                  id="tab-github-btn" onclick="switchTab('github')">GitHub Library</a>
<a href="#"                  id="tab-bambuman-btn" onclick="switchTab('bambuman')">BambuMan Library</a>
<a href="#"                  id="tab-status-btn" onclick="switchTab('status')">System</a>
<a href="#"                  id="tab-ota-btn"    onclick="switchTab('ota')">OTA Update</a>
<a href="#"                  id="tab-wifi-btn"   onclick="switchTab('wifi')">Config</a>
</nav>

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
<footer>&copy; 2026 by <a href="https://www.bambutagger.de" target="_blank">BambuTagger-Touch</a></footer>
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
  {
    static const char* const srcNames[] = {
      "bambu","tigertag","openspool","opentag3d","spoolease","unknown_ntag","unknown"
    };
    obj["tagSource"] = srcNames[(int)t->tagSource];
    if (t->tagUrl[0]) obj["tagUrl"] = t->tagUrl;
  }
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

// Stream-search /BM/catalog.json for a given UID; fill outMat/outType/outCol.

// Append a BM file path to /BM/index.txt (deduplicated).

// GET /api/bm/list – return index of downloaded BM files; prune stale entries.

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
  lcd.fillScreen(COL_BG);
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

// Decompress raw deflate data from stream, returns true on success

// Returns URL of today's (or recent) bambuman.ee daily ZIP


// POST /api/bm/sync – download full ZIP, extract data.bin files, build catalog

// GET /api/bm/catalog – serve /BM/catalog.json

// ── BambuMan per-tag download (/api/bm/fetch?uid=XXXXXXXX) ───

// ── Write tag from FAT dump via REST (/api/writetag) ───────────────────────

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

// Draw a progress bar on the touchscreen (pct 0-100)
void drawProgressBar(int pct, const char* phase, const char* label) {
  lcd.fillScreen(COL_BG);
  drawStatusBar(); drawSubHeader(phase);
  lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(3);
  lcd.setCursor(10, 125); lcd.print(label);
  int barW = LCD_WIDTH - 40, barH = 40, barX = 20, barY = 200;
  lcd.fillRoundRect(barX, barY, barW, barH, 4, COL_CARD);
  lcd.drawRoundRect(barX, barY, barW, barH, 4, COL_SLATE);
  int fill = (int)((long)pct * (barW - 4) / 100);
  if (fill > 0) lcd.fillRoundRect(barX + 2, barY + 2, fill, barH - 4, 3, COL_ACC);
  char pctStr[16]; snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
  lcd.setTextColor(COL_TEXT, COL_BG);
  lcd.setCursor(LCD_WIDTH / 2 - 20, barY + barH + 15);
  lcd.print(pctStr);
  drawFooter(); lcd.display();
}



// Sector-progress callback — called by rfidWriteDump() after each sector

// Internal OTA flash — used by both OLED flow and web API
// Returns empty string on success, error message on failure

void enterMainMenu() {
  DBGLN("[STATE] -> MAIN_MENU");
  appState = S_MAIN_MENU;
  ledOff();
  drawMenu();
}

// ──────────────────────────────────────────────────────────────
//  Screen / menu implementations
// ──────────────────────────────────────────────────────────────
#include "ui/screen_readtag.h"
#include "ui/screen_clonetag.h"
#include "ui/screen_tagtool.h"
#include "ui/screen_writetag.h"
#include "ui/screen_ghlib.h"
#include "ui/screen_bmcat.h"
#include "ui/screen_system.h"
#include "ui/screen_ota.h"

// forward declaration (defined below with apiOtaCheck)
static bool semverGt(const String& a, const String& b);

// OLED-driven blocking OTA flow

// Compare two bare semver strings (no leading 'v').  Returns true if b > a.

// GET /api/ota/check  — returns current + latest version info

// POST /api/ota/update  — download and flash, then reboot

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

// Load one directory level into fatEntries[]: dirs first, then .bin files.

// Navigate browser state to the parent dir of filePath and pre-select it.
// Does NOT redraw – caller does that.

// ──────────────────────────────────────────────────────────────
//  State-machine entry points
// ──────────────────────────────────────────────────────────────

// ══════════════════════════════════════════════════════════════
//  GitHub OLED browser
// ══════════════════════════════════════════════════════════════

// Fetch one directory level from the GitHub Contents API.
// Fills ghEntries[] / ghCount.  Returns true on success.

// Parse a GitHub dump.json into raw MIFARE binary (DUMP_SIZE bytes).
// Supports:
//   - Array of 64 hex strings (16 chars each)
//   - Object {"0":"hex...","1":"hex...",...}
//   - Object {"blocks": <above>}
//   - Object {"Cards":[{"Blocks":{...}}]}
// Returns number of bytes written (DUMP_SIZE on success, 0 on failure).

// Download a raw URL and save to FFat.  If it's a JSON file, parse it to
// binary first and save as .bin.  Returns true on success.

// Draw the GitHub browser screen

// Enter the GitHub browser at a given repo path.
// push=true saves current depth to stack (for BACK navigation).







// ──────────────────────────────────────────────────────────────
//  Main-menu encoder handler  (non-blocking)
// ──────────────────────────────────────────────────────────────
// ──────────────────────────────────────────────────────────────
//  BambuMan catalog OLED browser  (4-level: Mat→Type→Color→UID)
// ──────────────────────────────────────────────────────────────

// ── BambuMan cache helpers ─────────────────────────────────────────────────

// Single-pass stream build of all 3 navigation levels from /BM/catalog.json.
// Called automatically on first OLED browse; returns false if file missing.

// Populate bmCatEntries[] for the current browse level.
// Levels 0–2: served from RAM cache (fast, no SD read).
// Level 3 (UIDs): stream-parsed from catalog.json (list is unbounded).
// Returns false only if /BM/catalog.json is missing entirely.


// (Re-)enter the catalog browser at the given level; loads entries from FAT.
// ── OLED-driven BambuMan catalog sync ─────────────────────────────────────
// Quick sync: fetch central directory only via Range request, build catalog.json

// Full download: fetches entire ZIP and extracts data.bin files
// Downloads the full daily ZIP and extracts data.bin files into the FAT
// directory structure. Builds /BM/catalog.json for the 4-level browser.

// Fetch a dump from bambuman.ee by UID. Returns saved path or "" on error.
// Caller must show confirmation / error feedback.

// ── Keep legacy scan-by-tag flow for programmatic use ─────────────────────────




// ──────────────────────────────────────────────────────────────
//  Tag Tool  — standalone Seal / Unlock flow
// ──────────────────────────────────────────────────────────────
// ── OpenSpool creation ────────────────────────────────────────

// ── OpenTag3D creation ───────────────────────────────────────

// ── TigerTag creation ──────────────────────────────────────────


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
      lcd.fillScreen(COL_BG);
      lcd.setTextColor(COL_TEXT, COL_BG);
      lcd.setTextDatum(MC_DATUM);
      lcd.setTextSize(2);
      lcd.drawString("Tap each target to calibrate touch", LCD_WIDTH / 2, LCD_HEIGHT / 2);
      delay(1500);
      lcd.calibrateTouch(calParams, COL_RED, COL_BG, 10);
      touchCalSave(calParams);
      DBGLN("touch cal saved");
    }
  }

  // ── Pre‑compose logo (replace trans_key with COL_SIDEBAR) ─
  // The RGB panel requires swap565_t byte order; our data is rgb565_t.
  // Byte‑swap each pixel so the framebuffer gets the correct byte layout.
  logoBuffer = (uint16_t*)heap_caps_malloc(LOGO_W * LOGO_H * 2, MALLOC_CAP_DMA);
  if (logoBuffer) {
    for (int i = 0; i < LOGO_W * LOGO_H; i++) {
      uint16_t p = logoBitmap[i];
      uint16_t raw = (p == LOGO_TRANS_KEY) ? COL_SIDEBAR : p;
      logoBuffer[i] = __builtin_bswap16(raw);
    }
    DBGF("logoBuffer: %u bytes\n", LOGO_W * LOGO_H * 2);
  } else {
    DBGLN("logoBuffer: FAIL");
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


// ──────────────────────────────────────────────────────────────
//  Touch-drag scroll helpers for browsers
// ──────────────────────────────────────────────────────────────

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
                case 2: enterWriteTag(); break;
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
            lcd.fillScreen(COL_BG);
            drawStatusBar(); drawSubHeader("System");
            lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(3);
            lcd.setTextDatum(MC_DATUM);
            lcd.drawString("Delete ALL tags?", LCD_WIDTH / 2, 160);
            lcd.drawString("This cannot be undone!", LCD_WIDTH / 2, 200);
            lcd.setTextSize(2);
            drawBtn(200, 280, 400, 56, COL_RED, "Yes, delete all");
            drawBtn(200, 350, 400, 56, COL_CARD, "Cancel");
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
