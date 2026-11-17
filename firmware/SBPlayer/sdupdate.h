#pragma once
// Firmware and web-page updates from the SD card, applied at boot.
//
// tools/sb_card_builder (Python, Flutter or Qt) copies these to the card root:
//   /littlefs.bin   LittleFS image of firmware/SBPlayer/data (the web pages)
//   /firmware.bin   application image (SBPlayer.ino.bin, not the merged .bin)
//
// Each file is renamed afterwards - to *.installed.bin on success, *.failed.bin
// otherwise - so a bad file is tried once and never causes a boot loop. The result
// of every attempt is appended to /update.log on the card. The device settings
// (config.json, outputs.json) live on LittleFS too and are kept across a web-page
// update. The device restarts after anything was installed.
#include <Arduino.h>
#include <LittleFS.h>
#include <SD.h>
#include <Update.h>
#include "config.h"

#define SD_FIRMWARE_FILE "/firmware.bin"
#define SD_LITTLEFS_FILE "/littlefs.bin"
#define SD_UPDATE_LOG    "/update.log"

static void sdUpdateLog(const String &line) {
    Serial.println("[Update] " + line);
    File log = SD.open(SD_UPDATE_LOG, FILE_APPEND);
    if (log) { log.println(line); log.close(); }
}

// Renames <base>.bin to <base>.<suffix>.bin, replacing an older one.
static void sdUpdateRetire(const char *path, const char *suffix) {
    String to = String(path);
    to.replace(".bin", String(".") + suffix + ".bin");
    SD.remove(to);
    SD.rename(path, to);
}

// Writes one image from the SD card into the partition for `command` (U_FLASH or U_SPIFFS).
static bool sdUpdateFlash(const char *path, int command) {
    File f = SD.open(path);
    if (!f) { sdUpdateLog(String(path) + ": cannot open"); return false; }
    size_t size = f.size();
    bool ok = size > 0 && Update.begin(size, command);
    if (ok) ok = Update.writeStream(f) == size;
    if (ok) ok = Update.end();
    else    Update.abort();
    f.close();
    String line = String(path) + ": ";
    if (ok) line += "installed, " + String(size) + " bytes";
    else    line += String("FAILED - ") + Update.errorString();
    sdUpdateLog(line);
    return ok;
}

// Reads a whole LittleFS file; empty if it does not exist.
static String sdUpdateKeep(const char *path) {
    String s;
    File f = LittleFS.open(path, "r");
    if (f) { s = f.readString(); f.close(); }
    return s;
}

static void sdUpdateRestore(const char *path, const String &s) {
    if (s.isEmpty()) return;
    File f = LittleFS.open(path, "w");
    if (f) { f.print(s); f.close(); }
}

// Call after SD.begin() and LittleFS.begin(). Restarts the device if anything was installed.
static void applySdUpdates() {
    bool installed = false;

    if (SD.exists(SD_LITTLEFS_FILE)) {
        String config  = sdUpdateKeep(CONFIG_FILE);
        String outputs = sdUpdateKeep(OUTPUTS_FILE);
        LittleFS.end();
        bool ok = sdUpdateFlash(SD_LITTLEFS_FILE, U_SPIFFS);
        if (!LittleFS.begin(true)) Serial.println("[FS] LittleFS mount failed");
        sdUpdateRestore(CONFIG_FILE, config);
        sdUpdateRestore(OUTPUTS_FILE, outputs);
        sdUpdateRetire(SD_LITTLEFS_FILE, ok ? "installed" : "failed");
        installed |= ok;
    }

    if (SD.exists(SD_FIRMWARE_FILE)) {
        bool ok = sdUpdateFlash(SD_FIRMWARE_FILE, U_FLASH);
        sdUpdateRetire(SD_FIRMWARE_FILE, ok ? "installed" : "failed");
        installed |= ok;
    }

    if (installed) {
        sdUpdateLog("restarting");
        SD.end();
        delay(200);
        ESP.restart();
    }
}
