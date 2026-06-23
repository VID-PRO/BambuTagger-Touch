// screen_readtag.h — extracted from main.cpp

void drawTagInfo(const TagInfo* t, int) {
  lcd.fillScreen(TFT_BLACK);
  drawStatusBar();
  {
    const char* srcLabel;
    switch (t->tagSource) {
      case TAG_SRC_BAMBU:        srcLabel = "Bambu Tag";  break;
      case TAG_SRC_TIGERTAG:     srcLabel = "TigerTag";   break;
      case TAG_SRC_OPENSPOOL:    srcLabel = "OpenSpool";  break;
      case TAG_SRC_OPENTAG3D:   srcLabel = "OpenTag3D";  break;
      case TAG_SRC_SPOOLEASE:    srcLabel = "SpoolEase";  break;
      case TAG_SRC_UNKNOWN_NTAG: srcLabel = "NTAG";       break;
      default:                   srcLabel = "Tag Info";   break;
    }
    drawSubHeader(srcLabel);
  }

  lcd.setTextSize(3);

  int y = 125;
  int c1 = 10, c2 = 230;
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setCursor(c1, y); lcd.print("Type:");    lcd.setCursor(c2, y); lcd.print(t->filamentType); y += 26;
  lcd.setCursor(c1, y); lcd.print("Sub Type:");     lcd.setCursor(c2, y); lcd.print(t->detailedType); y += 26;
  lcd.setCursor(c1, y); lcd.print(t->tagSource == TAG_SRC_BAMBU ? "Variant:" : "Brand:"); lcd.setCursor(c2, y); lcd.print(t->variantId); y += 26;
  lcd.setCursor(c1, y); lcd.print(t->tagSource == TAG_SRC_BAMBU ? "Material ID:" : "Mat Label:"); lcd.setCursor(c2, y); lcd.print(t->materialId); y += 26;
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

void enterReadTag() {
  DBGLN("[STATE] -> READ_TAG");
  appState = S_READ_TAG;
  showStatus("Read Tag\nPlace Tag on reader");
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
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      MFRC522::PICC_Type pt = rfid.PICC_GetType(rfid.uid.sak);
      bool tagRead = false;
      memset(&currentTag, 0, sizeof(currentTag));

      if (pt == MFRC522::PICC_TYPE_MIFARE_UL) {
        currentTag.tagSource = TAG_SRC_UNKNOWN_NTAG;
        tagRead = rfidReadNTAGTag(&currentTag);
        if (!tagRead) rfid.PICC_HaltA();
      } else {
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
        rfid.PCD_AntennaOff();
        delay(30);
        rfid.PCD_AntennaOn();
        delay(20);
        tagRead = rfidReadBambuTag(&currentTag);
        if (tagRead) currentTag.tagSource = TAG_SRC_BAMBU;
      }

      if (tagRead) {
        DBGF("[RFID] Tag read OK: %s / %s  color=#%02X%02X%02X\n",
             currentTag.filamentType, currentTag.detailedType,
             currentTag.colorR, currentTag.colorG, currentTag.colorB);
        ledSetTagColor(&currentTag);
        appState = S_SHOW_TAG;
        drawTagInfo(&currentTag, 0);
        return;
      }
    }
    delay(18);
  }
  DBGLN("[RFID] processReadTag: timeout – no tag.");
  ledFlash(255, 0, 0, 2);
  showStatus("Read Tag\nNo tag detected.");
  appState = S_WIFI_INFO;
}
