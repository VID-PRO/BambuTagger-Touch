// screen_tagtool.h — extracted from main.cpp

struct PresetColor { const char* name; uint8_t r, g, b; };
static const PresetColor presetColors[] = {
  {"White", 240,240,240}, {"Black", 20,20,20}, {"Grey", 128,128,128},
  {"Red", 220,40,40}, {"Blue", 40,40,220}, {"Green", 40,180,40},
  {"Yellow", 240,220,40}, {"Orange", 240,140,20}, {"Purple", 160,40,200},
  {"Brown", 140,80,40}, {"Pink", 240,120,180}, {"Natural", 220,210,190},
};
static const int presetColorCount = 12;

static bool pickColor(uint8_t& r, uint8_t& g, uint8_t& b) {
  int sel = 0;
  while (true) {
    lcd.fillScreen(COL_BG);
    drawStatusBar(); drawSubHeader("Select Color");
    int cols = 4;
    int bw = 180, bh = 56, gapX = 16, gapY = 12;
    int x0 = (LCD_WIDTH - cols * (bw + gapX) + gapX) / 2;
    int y0 = 130;
    for (int i = 0; i < presetColorCount; i++) {
      int col = i % cols, row = i / cols;
      int x = x0 + col * (bw + gapX);
      int y = y0 + row * (bh + gapY);
      uint16_t color16 = lcd.color565(presetColors[i].r, presetColors[i].g, presetColors[i].b);
      uint16_t bg = (i == sel) ? COL_BLUE : color16;
      drawBtn(x, y, bw, bh, bg, presetColors[i].name);
    }
    drawFooter(); lcd.display();

    unsigned long t0 = millis();
    int tx = -1, ty = -1;
    while (millis() - t0 < 30000) {
      httpServer.handleClient();
      if (touchGet(&tx, &ty)) { while (touchGet(&tx, &ty)) { delay(10); } break; }
      delay(10);
    }
    if (tx < 0 || ty < 64) return false;

    for (int i = 0; i < presetColorCount; i++) {
      int col = i % cols, row = i / cols;
      int x = x0 + col * (bw + gapX);
      int y = y0 + row * (bh + gapY);
      if (tx >= x && tx <= x + bw && ty >= y && ty <= y + bh) {
        r = presetColors[i].r; g = presetColors[i].g; b = presetColors[i].b;
        return true;
      }
    }
  }
}
static bool pickTemp(uint16_t& tMin, uint16_t& tMax) {
  int sel = 3;  // default: 200-220 (PLA)
  static const uint16_t temps[][2] = {
    {180, 210},  // PLA low
    {200, 220},  // PLA
    {210, 230},  // PLA+
    {220, 250},  // PETG
    {230, 260},  // ABS
    {240, 270},  // ASA
    {250, 280},  // PC
    {260, 300},  // PA/PA-CF
  };
  static const char* tempLabels[] = {
    "PLA Low 180-210", "PLA 200-220", "PLA+ 210-230",
    "PETG 220-250", "ABS 230-260", "ASA 240-270",
    "PC 250-280", "PA 260-300",
  };
  const int tempCount = 8;

  while (true) {
    lcd.fillScreen(COL_BG);
    drawStatusBar(); drawSubHeader("Select Temperature");
    int bw = 380, bh = 52, gap = 8;
    int x0 = (LCD_WIDTH - 2 * bw) / 3;
    for (int i = 0; i < tempCount; i++) {
      int col = i % 2, row = i / 2;
      int x = x0 + col * (bw + gap);
      int y = 130 + row * (bh + gap);
      uint16_t bg = (i == sel) ? COL_BLUE : COL_CARD;
      drawBtn(x, y, bw, bh, bg, tempLabels[i]);
    }
    drawFooter(); lcd.display();

    unsigned long t0 = millis();
    int tx = -1, ty = -1;
    while (millis() - t0 < 30000) {
      httpServer.handleClient();
      if (touchGet(&tx, &ty)) { while (touchGet(&tx, &ty)) { delay(10); } break; }
      delay(10);
    }
    if (tx < 0 || ty < 64) return false;

    for (int i = 0; i < tempCount; i++) {
      int col = i % 2, row = i / 2;
      int x = x0 + col * (bw + gap);
      int y = 130 + row * (bh + gap);
      if (tx >= x && tx <= x + bw && ty >= y && ty <= y + bh) {
        tMin = temps[i][0]; tMax = temps[i][1];
        return true;
      }
    }
  }
}
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
