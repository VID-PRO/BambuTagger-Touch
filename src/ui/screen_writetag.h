// screen_writetag.h — extracted from main.cpp

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

inline int fatTotalRows() {
  return fatCount + (fatDepth > 0 ? 1 : 0);
}

// Forward declarations
void enterFatBrowser();
static void processCreateOpenTag3D();
static void processCreateOpenSpool();
static void processCreateTigerTag();

void drawFatBrowser() {
  lcd.fillScreen(COL_BG);

  drawStatusBar(); drawSubHeader("Write Tag");

  // Breadcrumb
  if (fatDepth > 0) {
    lcd.setTextColor(COL_SUBTEXT, COL_BG); lcd.setTextSize(2);
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
    drawBtn(bx, y, bw, bh, COL_CARD, label.c_str());
  }
  if (fatCount == 0 && fatDepth > 0) {
            int delY = LIST_ROW_Y0 + LIST_ROW_H;
    drawBtn(8, delY, LIST_BTN_W, LIST_BTN_H, COL_RED, "DELETE EMPTY FOLDER");
  }
  if (total > 0) drawScrollbar(scroll, total, LIST_ROW_Y0, LIST_MAX_VIS * LIST_ROW_H);
  drawFooter(); lcd.display();
}
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
void drawWriteScreen(const char* phase, int sectDone, int sectTotal) {
  int pct = (sectTotal > 0) ? (sectDone * 100 / sectTotal) : 0;
  char info[24];
  snprintf(info, sizeof(info), "%d / %d sec", sectDone, sectTotal);
  drawProgressBar(pct, "Write Tag", info);
}
static void writeProgressCbFn(int done, int total) {
  drawWriteScreen(g_webWrite ? "Web: writing..." : "writing...", done, total);
}
static String fatLastSeg(const char* fp) {
  String s(fp);
  int sl = s.lastIndexOf('/');
  return (sl >= 0) ? s.substring(sl + 1) : s;
}
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
void enterWriteTag() {
  lcd.fillScreen(COL_BG);
  drawStatusBar(); drawSubHeader("Write Tag");
  drawBtn(200, 125, 400, 62, COL_CARD, "Write Bambu Tag");
  drawBtn(200, 195, 400, 62, COL_CARD, "Create TigerTag");
  drawBtn(200, 265, 400, 62, COL_CARD, "Create OpenSpool");
  drawBtn(200, 335, 400, 62, COL_CARD, "Create OpenTag3D");
  drawFooter(); lcd.display();

  unsigned long t0 = millis();
  while (millis() - t0 < 15000) {
    httpServer.handleClient();
    int tx, ty;
    if (touchGet(&tx, &ty)) {
      while (touchGet(&tx, &ty)) { delay(10); }
      if (ty < 64) { enterMainMenu(); return; }
      if (tx >= 200 && tx <= 600) {
        if (ty >= 125 && ty <= 187) { enterFatBrowser(); return; }
        if (ty >= 195 && ty <= 257) { processCreateTigerTag(); return; }
        if (ty >= 265 && ty <= 327) { processCreateOpenSpool(); return; }
        if (ty >= 335 && ty <= 397) { processCreateOpenTag3D(); return; }
      }
    }
    delay(10);
  }
  enterMainMenu();
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
static void processCreateOpenSpool() {
  int matSel = 0, matScroll = 0;
  const int matCount = ttMaterialsCount;
  while (true) {
    lcd.fillScreen(COL_BG);
    drawStatusBar(); drawSubHeader("Select Material");
    int total = matCount;
    for (int i = 0; i < LIST_MAX_VIS && (matScroll + i) < total; i++) {
      int idx = matScroll + i;
      int y = LIST_ROW_Y0 + i * LIST_ROW_H;
      uint16_t bg = (idx == matSel) ? COL_BLUE : COL_CARD;
      drawBtn(8, y, LIST_BTN_W, LIST_BTN_H, bg, ttMaterials[idx].label);
    }
    if (total > LIST_MAX_VIS) drawScrollbar(matScroll, total);
    drawFooter(); lcd.display();

    unsigned long t0 = millis();
    int tx = -1, ty = -1;
    while (millis() - t0 < 30000) {
      httpServer.handleClient();
      if (touchGet(&tx, &ty)) {
        while (touchGet(&tx, &ty)) { delay(10); }
        break;
      }
      delay(10);
    }
    if (tx < 0) { enterMainMenu(); return; }
    if (ty < 64) { enterMainMenu(); return; }
    if (sbTapScroll(tx, ty, total, matScroll, SB_Y, SB_H)) continue;

    for (int i = 0; i < LIST_MAX_VIS && (matScroll + i) < total; i++) {
      int by = LIST_ROW_Y0 + i * LIST_ROW_H;
      if (tx >= 8 && tx <= 8 + LIST_BTN_W && ty >= by && ty <= by + LIST_BTN_H) {
        matSel = matScroll + i;
        const char* matLabel = ttMaterials[matSel].label;
        uint8_t r = 128, g = 128, b = 128;
        pickColor(r, g, b);
        uint16_t tMin = 200, tMax = 220;
        pickTemp(tMin, tMax);
        buildOpenSpool(matLabel, "Generic", r, g, b, 1000, 1.75f, tMin, tMax);

        showStatus("Write Tag\nPlace NTAG card\non reader");
        ledSet(255, 200, 0);
        unsigned long wt0 = millis();
        bool written = false;
        while (millis() - wt0 < 20000) {
          httpServer.handleClient();
          if (touchPoll()) { enterMainMenu(); return; }
          if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            written = rfidWriteNTAGPages(ntagWriteBuf, ntagWritePages);
            rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
            break;
          }
          delay(18);
        }
        if (written) {
          showStatus((String("Write Tag\nOpenSpool written!\n") + matLabel + "").c_str());
          ledFlash(0, 255, 0, 2);
        } else {
          showStatus("Write Tag\nWrite failed!\nTry NTAG card.");
          ledFlash(255, 0, 0, 2);
        }
        delay(3000);
        enterMainMenu();
        return;
      }
    }
  }
}
static void processCreateOpenTag3D() {
  int matSel = 0, matScroll = 0;
  const int matCount = ttMaterialsCount;
  while (true) {
    lcd.fillScreen(COL_BG);
    drawStatusBar(); drawSubHeader("Select Material");
    int total = matCount;
    for (int i = 0; i < LIST_MAX_VIS && (matScroll + i) < total; i++) {
      int idx = matScroll + i;
      int y = LIST_ROW_Y0 + i * LIST_ROW_H;
      uint16_t bg = (idx == matSel) ? COL_BLUE : COL_CARD;
      drawBtn(8, y, LIST_BTN_W, LIST_BTN_H, bg, ttMaterials[idx].label);
    }
    if (total > LIST_MAX_VIS) drawScrollbar(matScroll, total);
    drawFooter(); lcd.display();

    unsigned long t0 = millis();
    int tx = -1, ty = -1;
    while (millis() - t0 < 30000) {
      httpServer.handleClient();
      if (touchGet(&tx, &ty)) { while (touchGet(&tx, &ty)) { delay(10); } break; }
      delay(10);
    }
    if (tx < 0) { enterMainMenu(); return; }
    if (ty < 64) { enterMainMenu(); return; }
    if (sbTapScroll(tx, ty, total, matScroll, SB_Y, SB_H)) continue;

    for (int i = 0; i < LIST_MAX_VIS && (matScroll + i) < total; i++) {
      int by = LIST_ROW_Y0 + i * LIST_ROW_H;
      if (tx >= 8 && tx <= 8 + LIST_BTN_W && ty >= by && ty <= by + LIST_BTN_H) {
        matSel = matScroll + i;
        const char* matLabel = ttMaterials[matSel].label;
        uint8_t r = 128, g = 128, b = 128;
        pickColor(r, g, b);
        uint16_t tMin = 200, tMax = 220;
        pickTemp(tMin, tMax);
        buildOpenTag3D(matLabel, "Generic", r, g, b, 1000, 1.75f, tMin, tMax);

        showStatus("Write Tag\nPlace NTAG card\non reader");
        ledSet(255, 200, 0);
        unsigned long wt0 = millis();
        bool written = false;
        while (millis() - wt0 < 20000) {
          httpServer.handleClient();
          if (touchPoll()) { enterMainMenu(); return; }
          if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            written = rfidWriteNTAGPages(ntagWriteBuf, ntagWritePages);
            rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
            break;
          }
          delay(18);
        }
        if (written) {
          showStatus((String("Write Tag\nOpenTag3D written!\n") + matLabel + "").c_str());
          ledFlash(0, 255, 0, 2);
        } else {
          showStatus("Write Tag\nWrite failed!\nTry NTAG card.");
          ledFlash(255, 0, 0, 2);
        }
        delay(3000);
        enterMainMenu();
        return;
      }
    }
  }
}
static void processCreateTigerTag() {
  // Material selection
  int matSel = 0, matScroll = 0;
  const int matCount = ttMaterialsCount;
  while (true) {
    lcd.fillScreen(COL_BG);
    drawStatusBar(); drawSubHeader("Select Material");
    int total = matCount;
    for (int i = 0; i < LIST_MAX_VIS && (matScroll + i) < total; i++) {
      int idx = matScroll + i;
      int y = LIST_ROW_Y0 + i * LIST_ROW_H;
      uint16_t bg = (idx == matSel) ? COL_BLUE : COL_CARD;
      drawBtn(8, y, LIST_BTN_W, LIST_BTN_H, bg, ttMaterials[idx].label);
    }
    if (total > LIST_MAX_VIS) drawScrollbar(matScroll, total);
    drawFooter(); lcd.display();

    // Wait for input
    unsigned long t0 = millis();
    int tx = -1, ty = -1;
    while (millis() - t0 < 30000) {
      httpServer.handleClient();
      if (touchGet(&tx, &ty)) {
        while (touchGet(&tx, &ty)) { delay(10); }
        break;
      }
      delay(10);
    }
    if (tx < 0) { enterMainMenu(); return; }
    if (ty < 64) { enterMainMenu(); return; }

    // Check scrollbar tap
    if (sbTapScroll(tx, ty, total, matScroll, SB_Y, SB_H)) continue;

    // Check list rows
    for (int i = 0; i < LIST_MAX_VIS && (matScroll + i) < total; i++) {
      int by = LIST_ROW_Y0 + i * LIST_ROW_H;
      if (tx >= 8 && tx <= 8 + LIST_BTN_W && ty >= by && ty <= by + LIST_BTN_H) {
        matSel = matScroll + i;
        // Selected material – build and write
        uint16_t matId = (uint16_t)ttMaterials[matSel].id;
        uint16_t brandId = 65535;
        uint8_t r = 128, g = 128, b = 128;
        pickColor(r, g, b);
        uint16_t tMin = 200, tMax = 220;
        pickTemp(tMin, tMax);
        buildTigerTag(matId, brandId, r, g, b, 1000, 1.75f, tMin, tMax);

        // Write to NTAG
        showStatus("Tag Tool\nPlace NTAG card\non reader");
        ledSet(255, 200, 0);
        unsigned long wt0 = millis();
        bool written = false;
        while (millis() - wt0 < 20000) {
          httpServer.handleClient();
          if (touchPoll()) { enterMainMenu(); return; }
          if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
            written = rfidWriteNTAGPages(ntagWriteBuf, ntagWritePages);
            rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
            break;
          }
          delay(18);
        }
        if (written) {
          showStatus((String("Tag Tool\nTigerTag written!\n") + ttMaterials[matSel].label + "").c_str());
          ledFlash(0, 255, 0, 2);
        } else {
          showStatus("Tag Tool\nWrite failed!\nTry NTAG card.");
          ledFlash(255, 0, 0, 2);
        }
        delay(3000);
        enterMainMenu();
        return;
      }
    }
  }
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
static void scrollFatBrowser(int dir) {
  int total = fatTotalRows();
  int visRows = LIST_MAX_VIS;
  if (total <= visRows) return;
  fatScroll = constrain(fatScroll + dir, 0, total - visRows);
  fatSel = constrain(fatSel, fatScroll, fatScroll + visRows - 1);
  drawFatBrowser();
}
