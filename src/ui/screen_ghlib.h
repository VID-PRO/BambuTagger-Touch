// screen_ghlib.h — extracted from main.cpp

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
void drawGhBrowser() {
  lcd.fillScreen(COL_BG);

  drawStatusBar(); drawSubHeader("GitHub Library");

  // Breadcrumb
  if (ghDepth > 0) {
    lcd.setTextColor(COL_SUBTEXT, COL_BG); lcd.setTextSize(2);
    lcd.setCursor(12, 120);
    String crumb = ghStack[ghDepth - 1];
    if (crumb.length() > 44) crumb = "..." + crumb.substring(crumb.length() - 41);
    lcd.print(crumb);
  }

  int headerRows = (ghDepth == 0) ? 1 : 2;
  int totalRows  = ghCount + headerRows;

  if (totalRows == headerRows) {
    lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(3);
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
    drawBtn(8, y, bw, bh, COL_CARD, label.c_str());
  }
  drawScrollbar(ghScroll, totalRows, LIST_ROW_Y0, LIST_MAX_VIS * LIST_ROW_H);
  drawFooter(); lcd.display();
}
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
  lcd.fillScreen(COL_BG); drawStatusBar(); drawSubHeader("Downloading...");
  lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(2);
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
static void scrollGhBrowse(int dir) {
  int headerRows = (ghDepth == 0) ? 1 : 2;
  int totalRows = ghCount + headerRows;
  if (totalRows <= LIST_MAX_VIS) return;
  ghScroll = constrain(ghScroll + dir, 0, totalRows - LIST_MAX_VIS);
  ghSel = constrain(ghSel, ghScroll, ghScroll + LIST_MAX_VIS - 1);
  drawGhBrowser();
}
