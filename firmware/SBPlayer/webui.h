#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <LittleFS.h>
#include <SD.h>
#include <ETH.h>
#include <time.h>
#include <ArduinoJson.h>
#include "config.h"
#include "rtc.h"
#include "scheduler.h"

extern bool ethConnected;

// Shared playback state
struct PlaybackStatus {
    volatile bool     playing    = false;
    volatile uint32_t frame      = 0;
    volatile uint32_t frameCount = 0;
    volatile uint8_t  fps        = 0;
    char              seqFile[128]  = {0};
    char              audioFile[64] = {0};
    volatile bool     audioPlaying  = false;
};

struct PlayCommand {
    bool stop    = false;
    char file[128] = {0};
};

extern PlaybackStatus    gStatus;
extern PlayCommand       gPlayCmd;
extern volatile bool     gPlayCmdPending;
extern volatile bool     gRebootPending;

// Helper: serialize a JsonDocument directly into an HTTP response stream
static void _sendJson(AsyncWebServerRequest *req, JsonDocument &doc, int code = 200) {
    AsyncResponseStream *resp = req->beginResponseStream("application/json");
    serializeJson(doc, *resp);
    req->send(resp);
}

class WebUI {
public:
    void begin(Config &cfg, OutputsCfg &outputs, Scheduler &sched, RtcClock *rtc = nullptr) {
        _cfg     = &cfg;
        _outputs = &outputs;
        _sched   = &sched;
        _rtc     = rtc;

        _ws = new AsyncWebSocket("/ws");
        _ws->onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType, void*, uint8_t*, size_t){});
        _server.addHandler(_ws);

        // ── Status ────────────────────────────────────────────────────────────
        _server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req){
            JsonDocument doc;
            doc["playing"]       = gStatus.playing;
            doc["frame"]         = gStatus.frame;
            doc["frame_count"]   = gStatus.frameCount;
            doc["fps"]           = gStatus.fps;
            doc["sequence"]      = gStatus.seqFile;
            doc["audio"]         = gStatus.audioFile;
            doc["audio_playing"] = gStatus.audioPlaying;
            doc["eth_ip"]        = ethConnected ? ETH.localIP().toString().c_str() : "";
            doc["time_epoch"]    = (long)time(nullptr);
            _sendJson(req, doc);
        });

        // ── RTC GET ───────────────────────────────────────────────────────────
        _server.on("/api/rtc", HTTP_GET, [this](AsyncWebServerRequest *req){
            JsonDocument doc;
            bool avail = _rtc && _rtc->isAvailable();
            doc["available"] = avail;
            if (avail) doc["epoch"] = (long)_rtc->getTime();
            _sendJson(req, doc);
        });

        // ── RTC SET TIME ──────────────────────────────────────────────────────
        _server.on("/api/rtc/time", HTTP_POST,
            [](AsyncWebServerRequest *req){},
            nullptr,
            [this](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
                JsonDocument doc;
                if (deserializeJson(doc, data, len) || !doc["epoch"].is<long>()) {
                    req->send(400, "application/json", "{\"error\":\"bad request\"}"); return;
                }
                time_t t = (time_t)doc["epoch"].as<long>();
                if (_rtc && _rtc->isAvailable()) _rtc->setTime(t);
                struct timeval tv = { t, 0 };
                settimeofday(&tv, nullptr);
                req->send(200, "application/json", "{\"ok\":true}");
            });

        // ── Config GET ────────────────────────────────────────────────────────
        _server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *req){
            Serial.printf("[WebUI] GET /api/config (url=%s)\n", req->url().c_str());
            JsonDocument doc;
            configToJson(*_cfg, doc);
            _sendJson(req, doc);
        });

        // ── Config POST ───────────────────────────────────────────────────────
        _server.on("/api/config", HTTP_POST,
            [](AsyncWebServerRequest *req){},
            nullptr,
            [this](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
                JsonDocument doc;
                if (deserializeJson(doc, data, len)) {
                    req->send(400, "application/json", "{\"error\":\"parse\"}");
                    return;
                }
                configFromJson(*_cfg, doc);
                saveConfig(*_cfg);
                req->send(200, "application/json", "{\"ok\":true}");
            });

        // ── Sequences ─────────────────────────────────────────────────────────
        _server.on("/api/sequences", HTTP_GET, [](AsyncWebServerRequest *req){
            JsonDocument doc;
            bool sdOk = SD.cardType() != CARD_NONE;
            doc["sd_ok"] = sdOk;
            JsonArray arr = doc["files"].to<JsonArray>();
            if (sdOk) {
                File dir = SD.open("/sequences");
                if (dir) {
                    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
                        if (!f.isDirectory()) {
                            const char *name = f.name();
                            size_t n = strlen(name);
                            if (n > 5 && strcasecmp(name + n - 5, ".fseq") == 0)
                                arr.add(name);
                        }
                    }
                    dir.close();
                }
            }
            _sendJson(req, doc);
        });

        // ── Playlists GET (list or fetch one with ?name=) ─────────────────────
        _server.on("/api/playlists", HTTP_GET, [](AsyncWebServerRequest *req){
            if (req->hasParam("name")) {
                char path[128];
                snprintf(path, sizeof(path), "/playlists/%s",
                         req->getParam("name")->value().c_str());
                File f = SD.open(path);
                if (!f) { req->send(404, "application/json", "{\"error\":\"not found\"}"); return; }
                AsyncResponseStream *resp = req->beginResponseStream("application/json");
                while (f.available()) resp->write(f.read());
                f.close();
                req->send(resp);
            } else {
                JsonDocument doc;
                JsonArray arr = doc.to<JsonArray>();
                File dir = SD.open("/playlists");
                if (dir) {
                    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
                        if (!f.isDirectory()) arr.add(f.name());
                    }
                    dir.close();
                }
                _sendJson(req, doc);
            }
        });

        // ── Playlists POST ?name= ─────────────────────────────────────────────
        _server.on("/api/playlists", HTTP_POST,
            [](AsyncWebServerRequest *req){},
            nullptr,
            [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
                if (!req->hasParam("name", true)) {
                    req->send(400, "application/json", "{\"error\":\"missing name\"}"); return;
                }
                char path[128];
                snprintf(path, sizeof(path), "/playlists/%s",
                         req->getParam("name", true)->value().c_str());
                File f = SD.open(path, FILE_WRITE);
                if (!f) { req->send(500, "application/json", "{\"error\":\"write failed\"}"); return; }
                f.write(data, len);
                f.close();
                req->send(200, "application/json", "{\"ok\":true}");
            });

        // ── Schedule GET ──────────────────────────────────────────────────────
        _server.on("/api/schedule", HTTP_GET, [this](AsyncWebServerRequest *req){
            JsonDocument doc;
            JsonArray arr = doc["entries"].to<JsonArray>();
            for (int i = 0; i < _sched->scheduleCount; i++) {
                auto &se = _sched->schedule[i];
                JsonObject o = arr.add<JsonObject>();
                o["days"]       = se.days;
                o["start_hour"] = se.startHour;
                o["start_min"]  = se.startMin;
                o["end_hour"]   = se.endHour;
                o["end_min"]    = se.endMin;
                o["playlist"]   = se.playlist;
            }
            _sendJson(req, doc);
        });

        // ── Schedule POST ─────────────────────────────────────────────────────
        _server.on("/api/schedule", HTTP_POST,
            [](AsyncWebServerRequest *req){},
            nullptr,
            [this](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
                JsonDocument doc;
                if (deserializeJson(doc, data, len)) {
                    req->send(400, "application/json", "{\"error\":\"parse\"}"); return;
                }
                _sched->scheduleCount = 0;
                for (JsonVariantConst e : doc["entries"].as<JsonArrayConst>()) {
                    if (_sched->scheduleCount >= MAX_SCHEDULE_ENTRIES) break;
                    auto &se = _sched->schedule[_sched->scheduleCount++];
                    se.days      = e["days"]       | 0x7F;
                    se.startHour = e["start_hour"] | 0;
                    se.startMin  = e["start_min"]  | 0;
                    se.endHour   = e["end_hour"]   | 23;
                    se.endMin    = e["end_min"]    | 59;
                    strlcpy(se.playlist, e["playlist"] | "", sizeof(se.playlist));
                }
                _sched->saveSchedule();
                req->send(200, "application/json", "{\"ok\":true}");
            });

        // ── Outputs GET ───────────────────────────────────────────────────────
        _server.on("/api/outputs", HTTP_GET, [this](AsyncWebServerRequest *req){
            JsonDocument doc;
            outputsToJson(*_outputs, doc);
            _sendJson(req, doc);
        });

        // ── Outputs POST ──────────────────────────────────────────────────────
        _server.on("/api/outputs", HTTP_POST,
            [](AsyncWebServerRequest *req){},
            nullptr,
            [this](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
                JsonDocument doc;
                if (deserializeJson(doc, data, len)) {
                    req->send(400, "application/json", "{\"error\":\"parse\"}"); return;
                }
                outputsFromJson(*_outputs, doc);
                saveOutputs(*_outputs);
                req->send(200, "application/json", "{\"ok\":true}");
            });

        // ── Play ──────────────────────────────────────────────────────────────
        _server.on("/api/play", HTTP_POST,
            [](AsyncWebServerRequest *req){},
            nullptr,
            [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
                JsonDocument doc;
                if (!deserializeJson(doc, data, len) && doc["file"].is<const char*>()) {
                    strlcpy(gPlayCmd.file, doc["file"], sizeof(gPlayCmd.file));
                    gPlayCmd.stop   = false;
                    gPlayCmdPending = true;
                    req->send(200, "application/json", "{\"ok\":true}");
                } else {
                    req->send(400, "application/json", "{\"error\":\"missing file\"}");
                }
            });

        // ── Stop ──────────────────────────────────────────────────────────────
        _server.on("/api/stop", HTTP_POST, [](AsyncWebServerRequest *req){
            gPlayCmd.stop   = true;
            gPlayCmdPending = true;
            req->send(200, "application/json", "{\"ok\":true}");
        });

        // ── Reboot (flag → main loop calls ESP.restart()) ─────────────────────
        _server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *req){
            req->send(200, "application/json", "{\"ok\":true}");
            gRebootPending = true;
        });

        // ── OTA: firmware upload ───────────────────────────────────────────────
        _server.on("/update", HTTP_POST,
            [](AsyncWebServerRequest *req){
                bool ok = !Update.hasError();
                AsyncWebServerResponse *r = req->beginResponse(
                    200, "text/plain", ok ? "OK" : Update.errorString());
                r->addHeader("Connection", "close");
                req->send(r);
                if (ok) gRebootPending = true;
            },
            [](AsyncWebServerRequest *req, const String &filename,
               size_t index, uint8_t *data, size_t len, bool final){
                if (!index) {
                    Serial.printf("[OTA] Firmware: %s\n", filename.c_str());
                    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
                        Update.printError(Serial);
                }
                if (Update.isRunning() && Update.write(data, len) != len)
                    Update.printError(Serial);
                if (final) {
                    if (!Update.end(true)) Update.printError(Serial);
                    else Serial.printf("[OTA] Firmware done: %u bytes\n", index + len);
                }
            });

        // ── OTA: filesystem upload ────────────────────────────────────────────
        _server.on("/update/fs", HTTP_POST,
            [](AsyncWebServerRequest *req){
                bool ok = !Update.hasError();
                AsyncWebServerResponse *r = req->beginResponse(
                    200, "text/plain", ok ? "OK" : Update.errorString());
                r->addHeader("Connection", "close");
                req->send(r);
                if (ok) gRebootPending = true;
            },
            [](AsyncWebServerRequest *req, const String &filename,
               size_t index, uint8_t *data, size_t len, bool final){
                if (!index) {
                    Serial.printf("[OTA] Filesystem: %s\n", filename.c_str());
                    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS))
                        Update.printError(Serial);
                }
                if (Update.isRunning() && Update.write(data, len) != len)
                    Update.printError(Serial);
                if (final) {
                    if (!Update.end(true)) Update.printError(Serial);
                    else Serial.printf("[OTA] Filesystem done: %u bytes\n", index + len);
                }
            });

        // Serve all HTML from LittleFS via onNotFound — avoids any handler-order conflicts.
        // API routes registered above always take priority over this catch-all.
        _server.onNotFound([](AsyncWebServerRequest *req){
            if (req->method() != HTTP_GET) {
                req->send(404, "text/plain", "Not found");
                return;
            }
            String path = req->url();
            if (path == "/") path = "/index.html";

            Serial.printf("[WebUI] static %s\n", path.c_str());

            if (!LittleFS.exists(path)) {
                req->send(404, "text/plain", "Not found");
                return;
            }
            AsyncWebServerResponse *resp = req->beginResponse(LittleFS, path, "text/html");
            resp->addHeader("Cache-Control", "no-store");
            req->send(resp);
        });

        _server.begin();
        Serial.println("[WebUI] Started on port 80");
    }

    // Push live status to all WebSocket clients (~2 Hz from main loop)
    void pushStatus() {
        if (!_ws || _ws->count() == 0) return;
        JsonDocument doc;
        doc["playing"]       = gStatus.playing;
        doc["frame"]         = gStatus.frame;
        doc["frame_count"]   = gStatus.frameCount;
        doc["fps"]           = gStatus.fps;
        doc["sequence"]      = gStatus.seqFile;
        doc["audio_playing"] = gStatus.audioPlaying;
        doc["eth_ip"]        = ethConnected ? ETH.localIP().toString().c_str() : "";
        doc["time_epoch"]    = (long)time(nullptr);
        String out;
        serializeJson(doc, out);
        _ws->textAll(out);
    }

    void cleanupWs() { if (_ws) _ws->cleanupClients(); }

private:
    AsyncWebServer  _server{80};
    AsyncWebSocket *_ws      = nullptr;
    Config         *_cfg     = nullptr;
    OutputsCfg     *_outputs = nullptr;
    Scheduler      *_sched   = nullptr;
    RtcClock       *_rtc     = nullptr;
};
