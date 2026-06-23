// screen_clonetag.h — extracted from main.cpp

void enterCloneSource() {
  DBGLN("[STATE] -> CLONE_SOURCE");
  appState = S_CLONE_SOURCE;
  showStatus("Clone Tag\nCLONE  Step 1/2\nPlace SOURCE tag\non reader");
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
    if (rfidDetectAndReadTag(&sourceTag)) {
      DBGF("[CLONE] Source tag read: %s / %s  UID=%02X%02X%02X%02X\n",
           sourceTag.filamentType, sourceTag.detailedType,
           sourceTag.uid[0], sourceTag.uid[1],
           sourceTag.uid[2], sourceTag.uid[3]);
      ledSetTagColor(&sourceTag);
      if (sourceTag.tagSource == TAG_SRC_BAMBU) {
        tagToFlat(&sourceTag, dumpBuf);
      }
      // NTAG data already saved in ntagWriteBuf by reader
      showStatus2("Source read OK!", "Place TARGET card\x85");
      delay(1500);
      ledSet(255, 165, 0);
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
      ledSet(255, 255, 0);
      showStatus("Clone Tag\nWriting");

      bool writeOk = false;
      char cloneMsg[64];

      if (sourceTag.tagSource == TAG_SRC_TIGERTAG && ntagWritePages > 0) {
        // NTAG TigerTag clone
        writeOk = rfidWriteNTAGPages(ntagWriteBuf, ntagWritePages);
        if (writeOk) snprintf(cloneMsg, sizeof(cloneMsg), "TigerTag cloned!");
        else snprintf(cloneMsg, sizeof(cloneMsg), "NTAG write failed!\nTry another NTAG card.");
      } else {
        // MIFARE Classic clone
        int sectOk = rfidWriteDump(dumpBuf, true);
        DBGF("[CLONE] Write result: %d/%d sectors OK\n", sectOk, NUM_SECTORS);
        writeOk = (sectOk == NUM_SECTORS);
        bool partial = (sectOk > 0 && sectOk < NUM_SECTORS);
        if (writeOk)
          snprintf(cloneMsg, sizeof(cloneMsg), "Clone complete!");
        else if (partial)
          snprintf(cloneMsg, sizeof(cloneMsg), "Partial! %d/16 sec\nCard already keyed?", sectOk);
        else
          snprintf(cloneMsg, sizeof(cloneMsg), "Write failed!\nTry a magic/FUID\ncard.");
      }

      if (writeOk) ledFlash(0, 255, 0, 3);
      else ledFlash(255, 0, 0, 3);

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
