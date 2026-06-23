// screen_bmcat.h — extracted from main.cpp

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
String buildBmFilePath(const String& m, const String& t, const String& c, const String& uid) {
  return "/" + m + "/" + t + "/" + c + "/" + uid + ".bin";
}
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
void apiBmSync() {
  if (!WiFi.isConnected()) {
    httpServer.send(503, "application/json", "{\"error\":\"No WiFi\"}");
    return;
  }

  // Show status on display
  lcd.fillScreen(COL_BG);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(3);
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
      lcd.fillScreen(COL_BG);
      drawStatusBar(); drawSubHeader("BambuMan Sync");
      lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(3);
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
  lcd.fillScreen(COL_BG);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(3);
  lcd.setCursor(10, 138); lcd.print("Done!");
  lcd.setCursor(10, 166); lcd.print(String(count) + " files");
  drawFooter(); lcd.display();
  ledSet(0, 0, 40);

  String resp = "{\"ok\":true,\"count\":" + String(count) + "}";
  httpServer.send(200, "application/json", resp);
}
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
  lcd.fillScreen(COL_BG);

  drawStatusBar(); drawSubHeader("BambuMan Library");

  // Breadcrumb
  if (bmCatLevel > 0) {
    lcd.setTextColor(COL_SUBTEXT, COL_BG); lcd.setTextSize(2);
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
    drawBtn(8,  btnY, 384, 46, COL_CARD, "Sync Catalog");
    drawBtn(400, btnY, 384, 46, COL_CARD, "Full Download");
    listY0 = btnY + 56;
  }

  int syncExtra = 0;
  int backExtra = (bmCatLevel > 0) ? 1 : 0;
  int totalRows = bmCatCount + syncExtra + backExtra;

  int visRows = LIST_MAX_VIS;
  if (bmCatLevel == 0) visRows = min(LIST_MAX_VIS, (FOOTER_Y - listY0) / LIST_ROW_H);

  if (bmCatLevel == 0 && bmCatCount == 0) {
    lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(3);
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
    drawBtn(8, y, bw, bh, COL_CARD, label.c_str());
  }
  int scrollH = visRows * LIST_ROW_H;
  drawScrollbar(bmCatScroll, totalRows, listY0, scrollH);
  drawFooter(); lcd.display();
}
void bmOledSyncCatalogQuick() {
  if (WiFi.status() != WL_CONNECTED) {
    showStatus2("BambuMan Sync", "No WiFi!");
    delay(2000);
    enterBmCatBrowse(0);
    return;
  }

  lcd.fillScreen(COL_BG);
  drawStatusBar(); drawSubHeader("BambuMan Sync");
  lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(3);
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

  lcd.fillScreen(COL_BG);
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

  lcd.fillScreen(COL_BG);
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
  lcd.fillScreen(COL_BG);
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
void bmOledSyncCatalog() {
  if (WiFi.status() != WL_CONNECTED) {
    showStatus2("BambuMan Sync", "No WiFi!");
    delay(2000);
    enterBmCatBrowse(0);
    return;
  }

  // Step 1 – find ZIP URL
  lcd.fillScreen(COL_BG);
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
  lcd.fillScreen(COL_BG);
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
  lcd.fillScreen(COL_BG);
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
      lcd.fillScreen(COL_BG);
      drawStatusBar(); drawSubHeader("BambuMan Sync");
      lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(3);
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
  lcd.fillScreen(COL_BG);
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
