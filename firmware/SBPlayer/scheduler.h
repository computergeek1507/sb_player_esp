#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <time.h>

#define SCHEDULE_FILE "/schedule.json"
#define MAX_SCHEDULE_ENTRIES 32
#define MAX_PLAYLIST_ENTRIES 64

struct PlaylistEntry {
    char file[128];
};

struct ScheduleEntry {
    uint8_t days;       // bitmask: bit0=Sun, bit1=Mon, ..., bit6=Sat
    uint8_t startHour;
    uint8_t startMin;
    uint8_t endHour;
    uint8_t endMin;
    char    playlist[64];
};

class Scheduler {
public:
    PlaylistEntry playlist[MAX_PLAYLIST_ENTRIES];
    int  playlistCount  = 0;
    int  playlistIndex  = 0;
    bool playlistRepeat = false;
    bool playlistShuffle = false;

    ScheduleEntry schedule[MAX_SCHEDULE_ENTRIES];
    int scheduleCount = 0;

    // Playlist started at power-on without waiting for a valid clock, and played
    // whenever no schedule window is active. Empty = disabled.
    char bootPlaylist[64] = {0};

    bool loadSchedule() {
        File f = SD.open(SCHEDULE_FILE);
        if (!f) { Serial.println("[Sched] No schedule.json"); return false; }
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) { Serial.println("[Sched] Parse error"); return false; }
        strlcpy(bootPlaylist, doc["boot_playlist"] | "", sizeof(bootPlaylist));
        scheduleCount = 0;
        for (JsonVariantConst e : doc["entries"].as<JsonArrayConst>()) {
            if (scheduleCount >= MAX_SCHEDULE_ENTRIES) break;
            ScheduleEntry &se = schedule[scheduleCount++];
            se.days     = e["days"] | 0x7F;
            se.startHour = e["start_hour"] | 18;
            se.startMin  = e["start_min"]  | 0;
            se.endHour   = e["end_hour"]   | 23;
            se.endMin    = e["end_min"]    | 0;
            strlcpy(se.playlist, e["playlist"] | "default.json", sizeof(se.playlist));
        }
        Serial.printf("[Sched] Loaded %d entries\n", scheduleCount);
        return true;
    }

    bool saveSchedule() {
        JsonDocument doc;
        doc["boot_playlist"] = bootPlaylist;
        JsonArray arr = doc["entries"].to<JsonArray>();
        for (int i = 0; i < scheduleCount; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["days"]       = schedule[i].days;
            o["start_hour"] = schedule[i].startHour;
            o["start_min"]  = schedule[i].startMin;
            o["end_hour"]   = schedule[i].endHour;
            o["end_min"]    = schedule[i].endMin;
            o["playlist"]   = schedule[i].playlist;
        }
        File f = SD.open(SCHEDULE_FILE, FILE_WRITE);
        if (!f) return false;
        serializeJson(doc, f);
        f.close();
        return true;
    }

    bool loadPlaylist(const char *name) {
        char path[128];
        snprintf(path, sizeof(path), "/playlists/%s", name);
        File f = SD.open(path);
        if (!f) { Serial.printf("[Sched] Playlist not found: %s\n", path); return false; }
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) return false;
        playlistRepeat  = doc["repeat"]  | false;
        playlistShuffle = doc["shuffle"] | false;
        playlistCount   = 0;
        playlistIndex   = 0;
        for (JsonVariantConst e : doc["entries"].as<JsonArrayConst>()) {
            if (playlistCount >= MAX_PLAYLIST_ENTRIES) break;
            strlcpy(playlist[playlistCount++].file, e | "", 128);
        }
        Serial.printf("[Sched] Playlist %s: %d entries\n", name, playlistCount);
        return playlistCount > 0;
    }

    // Returns path to next sequence file, or nullptr if playlist exhausted
    const char *nextSequence() {
        if (playlistCount == 0) return nullptr;
        if (playlistIndex >= playlistCount) {
            if (!playlistRepeat) return nullptr;
            playlistIndex = 0;
        }
        return playlist[playlistIndex++].file;
    }

    // Check current time against schedule; load playlist if a window is active.
    // Returns true if a schedule window is active.
    bool checkSchedule() {
        time_t now = time(nullptr);
        struct tm *t = localtime(&now);
        if (!t || t->tm_year < 120) return false;  // NTP not synced yet

        uint8_t dowBit = 1 << t->tm_wday;
        uint16_t nowMin = t->tm_hour * 60 + t->tm_min;

        for (int i = 0; i < scheduleCount; i++) {
            ScheduleEntry &se = schedule[i];
            if (!(se.days & dowBit)) continue;
            uint16_t startMin = se.startHour * 60 + se.startMin;
            uint16_t endMin   = se.endHour   * 60 + se.endMin;
            if (nowMin >= startMin && nowMin < endMin) {
                if (_activeSchedule != i) {
                    _activeSchedule = i;
                    loadPlaylist(se.playlist);
                    playlistIndex = 0;
                }
                return true;
            }
        }
        _activeSchedule = -1;
        return false;
    }

private:
    int _activeSchedule = -1;
};
