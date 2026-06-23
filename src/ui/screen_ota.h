// screen_ota.h — extracted from main.cpp

static bool semverGt(const String& a, const String& b);
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
void otaDrawProgress(int pct, const char* label) {
  drawProgressBar(pct, "OTA Firmware", label);
}
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
