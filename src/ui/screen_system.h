// screen_system.h — extracted from main.cpp

void enterWifiInfo() {
  DBGLN("[STATE] -> SYSTEM");
  appState = S_WIFI_INFO;
  lcd.fillScreen(COL_BG);
  drawStatusBar(); drawSubHeader("System");
  lcd.setTextColor(COL_TEXT, COL_BG); lcd.setTextSize(2);

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
  drawBtn(200, btnY, 400, 56, COL_RED, "Delete All Tags");

  drawFooter(); lcd.display();
}
