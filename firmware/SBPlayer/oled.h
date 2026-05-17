#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

class OledDisplay {
public:
    bool begin(uint8_t sdaPin, uint8_t sclPin) {
        Wire.begin(sdaPin, sclPin);
        _disp = new Adafruit_SSD1306(128, 64, &Wire, -1);
        if (!_disp->begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
            Serial.println("[OLED] Not found");
            delete _disp;
            _disp = nullptr;
            return false;
        }
        _disp->clearDisplay();
        _disp->setTextSize(1);
        _disp->setTextColor(SSD1306_WHITE);
        _disp->setCursor(0, 0);
        _disp->println("SB Player");
        _disp->println("Starting...");
        _disp->display();
        return true;
    }

    void update(const char *ipStr,
                const char *seqName,
                uint32_t frame, uint32_t frameCount,
                uint8_t fps,
                bool playing, bool scheduled,
                int e131Count, int ddpCount, bool syncActive,
                const char *audioName) {
        if (!_disp) return;

        _disp->clearDisplay();
        _disp->setTextSize(1);
        _disp->setCursor(0, 0);

        // Line 1: IP
        _disp->print(ipStr);
        _disp->println();

        // Line 2: State + sequence name (max 16 chars)
        if (playing) {
            char shortName[17] = {0};
            strncpy(shortName, seqName ? seqName : "?", 16);
            _disp->print(shortName);
        } else if (scheduled) {
            _disp->print("SCHEDULED");
        } else {
            _disp->print("STOPPED");
        }
        _disp->println();

        // Line 3: Frame counter + FPS
        if (playing) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lu/%lu FPS:%d", (unsigned long)frame, (unsigned long)frameCount, fps);
            _disp->println(buf);
        } else {
            _disp->println();
        }

        // Line 4: Active outputs
        char outBuf[32];
        snprintf(outBuf, sizeof(outBuf), "E1.31:%d DDP:%d%s",
                 e131Count, ddpCount, syncActive ? " SYNC" : "");
        _disp->println(outBuf);

        // Line 5: Current time (if synced)
        time_t now = time(nullptr);
        struct tm *t = localtime(&now);
        if (t && t->tm_year >= 120) {
            char timeBuf[12];
            strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", t);
            _disp->print(timeBuf);
        }

        if (audioName && audioName[0]) {
            _disp->setCursor(0, 56);
            char shortAudio[17] = {0};
            strncpy(shortAudio, audioName, 16);
            _disp->print(shortAudio);
        }

        _disp->display();
    }

    bool isAvailable() const { return _disp != nullptr; }

private:
    Adafruit_SSD1306 *_disp = nullptr;
};
