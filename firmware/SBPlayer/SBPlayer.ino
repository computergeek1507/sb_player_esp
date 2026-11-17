#include <Arduino.h>
#include <WiFi.h>
#include <WiFiAP.h>
#include <ETH.h>
#include <LittleFS.h>
#include <SD.h>
#include <time.h>
#include <esp_sntp.h>
#include "config.h"
#include "fseq.h"
#include "audio.h"
#include "e131.h"
#include "ddp.h"
#include "multisync.h"
#include "oled.h"
#include "rtc.h"
#include "scheduler.h"
#include "webui.h"

// ── Globals shared with webui.h ───────────────────────────────────────────────
PlaybackStatus gStatus;
PlayCommand    gPlayCmd;
volatile bool  gPlayCmdPending = false;
volatile bool  gRebootPending  = false;

// ── Subsystem objects ─────────────────────────────────────────────────────────
static Config      cfg;
static OutputsCfg  outputs;
static FseqPlayer  fseq;
static AudioPlayer audio;
static E131Sender  e131Senders[MAX_E131_OUTPUTS];
static DDPSender   ddpSenders[MAX_DDP_OUTPUTS];
static MultiSync   msync;
static OledDisplay oled;
static RtcClock    rtcClock;
static Scheduler   scheduler;
static WebUI       webui;

// ── FreeRTOS inter-task signaling ─────────────────────────────────────────────
static QueueHandle_t     seqDoneQueue;
static SemaphoreHandle_t fseqMutex;

// ── Ethernet state ────────────────────────────────────────────────────────────
bool ethConnected    = false;
static bool servicesStarted = false;

static void _startNetworkServices();  // forward declaration

static void ethEvent(arduino_event_id_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            ETH.setHostname(cfg.hostname);
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("[ETH] Link up");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            Serial.printf("[ETH] IP: %s\n", ETH.localIP().toString().c_str());
            ethConnected = true;
            _startNetworkServices();
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println("[ETH] Disconnected");
            ethConnected = false;
            break;
        default: break;
    }
}

// ── Network service init (called once Ethernet has an IP) ─────────────────────
static void _startNetworkServices() {
    if (servicesStarted) return;
    servicesStarted = true;

    IPAddress ethIp = ETH.localIP();
    uint8_t   mac[6];
    ETH.macAddress(mac);

    configTime(cfg.tzOffsetMins * 60, 0, cfg.ntpServer);
    Serial.printf("[NTP] Syncing from %s\n", cfg.ntpServer);

    // E1.31 outputs
    for (int i = 0; i < outputs.e131Count; i++) {
        E131OutCfg &o = outputs.e131[i];
        if (!o.enabled) continue;
        e131Senders[i].begin(o.universeStart, o.universeSize,
                             o.multicast, o.unicastIp, mac, ethIp);
        Serial.printf("[E1.31] Output %d: universe %d, ch %lu+%lu, %s\n",
                      i, o.universeStart, o.channelStart, o.channelCount,
                      o.multicast ? "multicast" : o.unicastIp);
    }

    // DDP outputs
    for (int i = 0; i < outputs.ddpCount; i++) {
        DDPOutCfg &o = outputs.ddp[i];
        if (!o.enabled) continue;
        ddpSenders[i].begin(o.ip, ethIp);
        Serial.printf("[DDP] Output %d: %s, ch %lu+%lu\n",
                      i, o.ip, o.channelStart, o.channelCount);
    }

    // MultiSync master
    if (cfg.multisyncEnabled) {
        msync.begin(cfg.multisyncPort, ethIp);
        Serial.println("[MultiSync] Ready");
    }
}

// ── Playback helpers ──────────────────────────────────────────────────────────
static uint8_t *frameBuf   = nullptr;
static uint32_t frameBufSz = 0;

static void playSequence(const char *path) {
    xSemaphoreTake(fseqMutex, portMAX_DELAY);
    fseq.close();
    char fullPath[140];
    snprintf(fullPath, sizeof(fullPath), "/sequences/%s", path);
    if (!fseq.open(fullPath)) {
        Serial.printf("[FSEQ] Failed to open: %s\n", fullPath);
        xSemaphoreGive(fseqMutex);
        return;
    }
    if (fseq.channelCount > frameBufSz) {
        free(frameBuf);
        frameBuf   = (uint8_t*)malloc(fseq.channelCount);
        frameBufSz = fseq.channelCount;
    }
    strlcpy(gStatus.seqFile, path, sizeof(gStatus.seqFile));
    gStatus.frameCount = fseq.frameCount;
    gStatus.frame      = 0;
    gStatus.playing    = true;
    xSemaphoreGive(fseqMutex);

    audio.stop();
    if (audio.openForSequence(fseq.mediaFile)) {
        strlcpy(gStatus.audioFile, fseq.mediaFile, sizeof(gStatus.audioFile));
        gStatus.audioPlaying = true;
    } else {
        gStatus.audioFile[0] = 0;
        gStatus.audioPlaying = false;
    }

    if (cfg.multisyncEnabled) msync.sendOpen(path);
    Serial.printf("[Play] %s  ch=%lu  frames=%lu  step=%dms\n",
                  path, fseq.channelCount, fseq.frameCount, fseq.stepTimeMs);
}

static void stopPlayback() {
    xSemaphoreTake(fseqMutex, portMAX_DELAY);
    if (gStatus.playing && cfg.multisyncEnabled)
        msync.sendStop(gStatus.seqFile);
    fseq.close();
    gStatus.playing    = false;
    gStatus.frame      = 0;
    gStatus.seqFile[0] = 0;
    xSemaphoreGive(fseqMutex);
    audio.stop();
    gStatus.audioPlaying = false;
}

// ── Playback task (core 0) ────────────────────────────────────────────────────
static void playbackTask(void *) {
    TickType_t lastWake  = xTaskGetTickCount();
    uint32_t fpsCounter  = 0;
    uint32_t fpsLastMs   = millis();

    while (true) {
        if (gPlayCmdPending) {
            gPlayCmdPending = false;
            if (gPlayCmd.stop)        stopPlayback();
            else if (gPlayCmd.file[0]) { stopPlayback(); playSequence(gPlayCmd.file); }
        }

        if (!gStatus.playing) {
            vTaskDelay(pdMS_TO_TICKS(10));
            lastWake = xTaskGetTickCount();
            continue;
        }

        xSemaphoreTake(fseqMutex, portMAX_DELAY);
        bool     ok     = fseq.isOpen() && !fseq.atEnd();
        uint32_t stepMs = fseq.stepTimeMs;
        uint32_t frame  = fseq.currentFrame;
        xSemaphoreGive(fseqMutex);

        if (!ok) {
            gStatus.playing = false;
            audio.stop();
            gStatus.audioPlaying = false;
            if (cfg.multisyncEnabled) msync.sendStop(gStatus.seqFile);
            uint8_t done = 1;
            xQueueSend(seqDoneQueue, &done, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            lastWake = xTaskGetTickCount();
            continue;
        }

        xSemaphoreTake(fseqMutex, portMAX_DELAY);
        bool     frameOk   = fseq.readFrame(frameBuf);
        uint32_t chanCount = fseq.channelCount;
        xSemaphoreGive(fseqMutex);

        if (frameOk) {
            // Send to all enabled E1.31 outputs
            for (int i = 0; i < outputs.e131Count; i++) {
                E131OutCfg &o = outputs.e131[i];
                if (o.enabled)
                    e131Senders[i].sendFrame(frameBuf, chanCount,
                                            o.channelStart, o.channelCount);
            }
            // Send to all enabled DDP outputs
            for (int i = 0; i < outputs.ddpCount; i++) {
                DDPOutCfg &o = outputs.ddp[i];
                if (o.enabled)
                    ddpSenders[i].sendFrame(frameBuf, chanCount,
                                           o.channelStart, o.channelCount);
            }
            if (cfg.multisyncEnabled) msync.sendSync(frame, gStatus.seqFile);

            gStatus.frame = frame + 1;
            fpsCounter++;
            uint32_t now = millis();
            if (now - fpsLastMs >= 1000) {
                gStatus.fps = fpsCounter;
                fpsCounter  = 0;
                fpsLastMs   = now;
            }
        }

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(stepMs));
    }
}

// ── Audio task (core 1) ───────────────────────────────────────────────────────
static void audioTask(void *) {
    while (true) { audio.loop(); vTaskDelay(1); }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("ESP32-P4 SB Player starting");

    if (!LittleFS.begin(true)) Serial.println("[FS] LittleFS mount failed");

    loadConfig(cfg);
    loadOutputs(outputs);
    Serial.printf("[Config] Hostname: %s  E1.31 outputs: %d  DDP outputs: %d\n",
                  cfg.hostname, outputs.e131Count, outputs.ddpCount);

    if (!SD.begin()) Serial.println("[SD] Mount failed — check card");
    else             Serial.printf("[SD] %.1f MB\n", SD.totalBytes() / 1048576.0);

    // One-shot import of /outputs.json from the SD card (written by tools/sb_card_builder.py).
    // The file is renamed afterwards so later web UI edits are not overwritten on the next boot.
    if (SD.exists("/outputs.json")) {
        File f = SD.open("/outputs.json");
        JsonDocument doc;
        bool parsed = f && !deserializeJson(doc, f);
        if (f) f.close();
        if (parsed) {
            outputsFromJson(outputs, doc);
            saveOutputs(outputs);
            SD.remove("/outputs.imported.json");
            SD.rename("/outputs.json", "/outputs.imported.json");
            Serial.printf("[Config] Imported outputs from SD: %d E1.31, %d DDP\n",
                          outputs.e131Count, outputs.ddpCount);
        } else {
            Serial.println("[Config] SD outputs.json could not be parsed - ignored");
        }
    }

    if (cfg.oledEnabled) oled.begin(cfg.oledSda, cfg.oledScl);

    if (cfg.rtcEnabled) {
        // OLED already called Wire.begin(); if OLED is off, start Wire here
        if (!cfg.oledEnabled) Wire.begin(cfg.oledSda, cfg.oledScl);
        if (rtcClock.begin(rtcTypeFromStr(cfg.rtcType), &Wire)) {
            time_t t = rtcClock.getTime();
            if (t) {
                struct timeval tv = { t, 0 };
                settimeofday(&tv, nullptr);
                Serial.println("[RTC] System time set from RTC");
            }
        }
    }

    audio.begin(cfg.i2sBclk, cfg.i2sLrclk, cfg.i2sData, cfg.audioVolume);

    bool apOk = WiFi.softAP(cfg.apSsid, cfg.apPassword[0] ? cfg.apPassword : nullptr);
    Serial.printf("[WiFi] AP %s: %s  IP: %s\n",
                  apOk ? "OK" : "FAILED",
                  cfg.apSsid, WiFi.softAPIP().toString().c_str());

    if (cfg.ethEnabled) {
        if (cfg.ethStatic) {
            IPAddress ip, gw, sn, dns;
            ip.fromString(cfg.ethIp); gw.fromString(cfg.ethGateway);
            sn.fromString(cfg.ethSubnet); dns.fromString(cfg.ethDns);
            ETH.config(ip, gw, sn, dns);
        }
        Network.onEvent(ethEvent);
        ETH.begin(ETH_PHY_TYPE, cfg.ethPhyAddr, cfg.ethMdcPin, cfg.ethMdioPin,
                  cfg.ethPowerPin, ETH_CLK_MODE);
    } else {
        Serial.println("[ETH] Disabled");
    }

    scheduler.loadSchedule();
    webui.begin(cfg, outputs, scheduler, &rtcClock);

    fseqMutex    = xSemaphoreCreateMutex();
    seqDoneQueue = xQueueCreate(4, sizeof(uint8_t));
    xTaskCreatePinnedToCore(playbackTask, "playback", 8192, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(audioTask,    "audio",    4096, nullptr, 5, nullptr, 1);

    Serial.println("[SB] Ready");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
static uint32_t lastOledMs    = 0;
static uint32_t lastSchedMs   = 0;
static uint32_t lastWsMs      = 0;
static uint32_t lastRtcSyncMs = 0;
static bool     scheduledPlay = false;
static bool     bootPlay      = false;   // playing scheduler.bootPlaylist (no clock needed)
static bool     bootChecked   = false;
static bool     rtcNtpSynced  = false;

// Load the boot playlist and queue its first sequence. Returns false if there is none.
static bool startBootPlaylist() {
    if (!scheduler.bootPlaylist[0] || !scheduler.loadPlaylist(scheduler.bootPlaylist)) return false;
    const char *next = scheduler.nextSequence();
    if (!next) return false;
    strlcpy(gPlayCmd.file, next, sizeof(gPlayCmd.file));
    gPlayCmd.stop   = false;
    gPlayCmdPending = true;
    bootPlay        = true;
    Serial.printf("[Sched] Boot playlist: %s\n", scheduler.bootPlaylist);
    return true;
}

void loop() {
    if (gRebootPending) { delay(100); ESP.restart(); }

    uint32_t now = millis();

    // Start the boot playlist once, unless the clock is already valid (RTC) and a
    // schedule window is active - the schedule check below starts that one instead.
    if (!bootChecked) {
        bootChecked = true;
        if (!scheduler.checkSchedule()) startBootPlaylist();
    }

    uint8_t done;
    while (xQueueReceive(seqDoneQueue, &done, 0) == pdTRUE) {
        if (scheduledPlay || bootPlay) {
            const char *next = scheduler.nextSequence();
            if (next) { strlcpy(gPlayCmd.file, next, sizeof(gPlayCmd.file)); gPlayCmd.stop=false; gPlayCmdPending=true; }
            else       scheduledPlay = bootPlay = false;
        }
    }

    if (now - lastSchedMs >= 10000) {
        lastSchedMs = now;
        bool inWindow = scheduler.checkSchedule();
        if (inWindow && bootPlay) {
            // checkSchedule() has loaded the scheduled playlist; it takes over when the
            // current sequence ends.
            bootPlay = false; scheduledPlay = true;
        } else if (inWindow && !gStatus.playing) {
            const char *next = scheduler.nextSequence();
            if (next) { scheduledPlay=true; strlcpy(gPlayCmd.file,next,sizeof(gPlayCmd.file)); gPlayCmd.stop=false; gPlayCmdPending=true; }
        } else if (!inWindow && scheduledPlay) {
            // Window ended: go back to the boot playlist if there is one, otherwise stop.
            scheduledPlay = false;
            if (!startBootPlaylist()) { gPlayCmd.stop=true; gPlayCmdPending=true; }
        }
    }

    // Write NTP time back to RTC once after first successful sync
    if (cfg.rtcEnabled && rtcClock.isAvailable() && !rtcNtpSynced && ethConnected) {
        if (now - lastRtcSyncMs >= 30000) {
            lastRtcSyncMs = now;
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                rtcClock.setTime(time(nullptr));
                rtcNtpSynced = true;
            }
        }
    }

    if (now - lastWsMs >= 500) {
        lastWsMs = now;
        webui.pushStatus();
        webui.cleanupWs();
    }

    if (now - lastOledMs >= 500) {
        lastOledMs = now;
        String ipStr = cfg.ethEnabled && ethConnected
            ? ETH.localIP().toString() : WiFi.softAPIP().toString();
        oled.update(ipStr.c_str(),
                    gStatus.seqFile, gStatus.frame, gStatus.frameCount, gStatus.fps,
                    gStatus.playing, scheduledPlay,
                    outputs.e131Count, outputs.ddpCount, cfg.multisyncEnabled,
                    gStatus.audioFile);
    }

    delay(10);
}
