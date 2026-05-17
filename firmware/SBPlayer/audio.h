#pragma once
#include <Arduino.h>
#include <SD.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorWAV.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

class AudioPlayer {
public:
    bool begin(uint8_t bclkPin, uint8_t lrclkPin, uint8_t dataPin, uint8_t volume) {
        _bclkPin  = bclkPin;
        _lrclkPin = lrclkPin;
        _dataPin  = dataPin;
        _volume   = volume;
        return true;
    }

    // Try to open matching audio for a FSEQ media filename.
    // mediaFile may be empty string — returns false silently.
    bool openForSequence(const char *mediaFile) {
        stop();
        if (!mediaFile || mediaFile[0] == 0) return false;

        // Strip path from media filename and build SD path
        char path[128];
        const char *name = strrchr(mediaFile, '/');
        name = name ? name + 1 : mediaFile;

        // Try WAV first, then MP3
        snprintf(path, sizeof(path), "/sequences/%s", name);
        _replaceExt(path, ".wav");
        if (SD.exists(path)) return _open(path, false);
        _replaceExt(path, ".mp3");
        if (SD.exists(path)) return _open(path, true);

        // Fallback: try exact name given in media field
        if (SD.exists(mediaFile)) {
            bool isMp3 = strstr(mediaFile, ".mp3") || strstr(mediaFile, ".MP3");
            return _open(mediaFile, isMp3);
        }
        Serial.printf("[Audio] No audio found for: %s\n", name);
        return false;
    }

    void loop() {
        if (!_running) return;
        if (_isMp3) {
            if (_mp3 && _mp3->isRunning()) { if (!_mp3->loop()) stop(); }
        } else {
            if (_wav && _wav->isRunning()) { if (!_wav->loop()) stop(); }
        }
    }

    void stop() {
        if (_isMp3 && _mp3) { _mp3->stop(); delete _mp3; _mp3 = nullptr; }
        if (!_isMp3 && _wav) { _wav->stop(); delete _wav; _wav = nullptr; }
        if (_out)  { _out->stop(); delete _out; _out = nullptr; }
        if (_src)  { delete _src; _src = nullptr; }
        _running = false;
    }

    bool isRunning() const { return _running; }

    void setVolume(uint8_t v) {
        _volume = v;
        if (_out) _out->SetGain(_volume / 100.0f);
    }

private:
    uint8_t _bclkPin = 26, _lrclkPin = 25, _dataPin = 22;
    uint8_t _volume  = 80;
    bool    _running = false;
    bool    _isMp3   = false;

    AudioFileSourceSD  *_src = nullptr;
    AudioOutputI2S     *_out = nullptr;
    AudioGeneratorWAV  *_wav = nullptr;
    AudioGeneratorMP3  *_mp3 = nullptr;

    bool _open(const char *path, bool mp3) {
        _src = new AudioFileSourceSD(path);
        _out = new AudioOutputI2S();
        _out->SetPinout(_bclkPin, _lrclkPin, _dataPin);
        _out->SetGain(_volume / 100.0f);
        _isMp3 = mp3;
        if (mp3) {
            _mp3 = new AudioGeneratorMP3();
            if (!_mp3->begin(_src, _out)) { stop(); return false; }
        } else {
            _wav = new AudioGeneratorWAV();
            if (!_wav->begin(_src, _out)) { stop(); return false; }
        }
        _running = true;
        Serial.printf("[Audio] Playing: %s\n", path);
        return true;
    }

    void _replaceExt(char *path, const char *ext) {
        char *dot = strrchr(path, '.');
        if (dot) strlcpy(dot, ext, 5);
        else strlcat(path, ext, 128);
    }
};
