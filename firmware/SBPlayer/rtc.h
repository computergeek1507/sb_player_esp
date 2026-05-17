#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <time.h>

// Minimum plausible unix timestamp (2020-01-01)
#define RTC_MIN_VALID_TIME 1577836800UL

enum class RtcType : uint8_t { DS3231 = 0, DS1307 = 1, PCF8523 = 2 };

inline RtcType rtcTypeFromStr(const char *s) {
    if (s && strcasecmp(s, "DS1307") == 0)  return RtcType::DS1307;
    if (s && strcasecmp(s, "PCF8523") == 0) return RtcType::PCF8523;
    return RtcType::DS3231;
}

class RtcClock {
public:
    bool begin(RtcType type, TwoWire *wire) {
        _type = type;
        bool ok = false;
        switch (type) {
            case RtcType::DS3231:  ok = _ds3231.begin(wire);  break;
            case RtcType::DS1307:  ok = _ds1307.begin(wire);  break;
            case RtcType::PCF8523: ok = _pcf8523.begin(wire); break;
        }
        if (!ok) { Serial.println("[RTC] Not found"); return false; }
        _available = true;
        time_t t = getTime();
        if (t) Serial.printf("[RTC] Ready, time=%lu\n", (unsigned long)t);
        else   Serial.println("[RTC] Ready (time not set)");
        return true;
    }

    time_t getTime() {
        if (!_available) return 0;
        DateTime now;
        switch (_type) {
            case RtcType::DS3231:  now = _ds3231.now();  break;
            case RtcType::DS1307:  now = _ds1307.now();  break;
            case RtcType::PCF8523: now = _pcf8523.now(); break;
        }
        return (now.unixtime() < RTC_MIN_VALID_TIME) ? 0 : now.unixtime();
    }

    void setTime(time_t t) {
        if (!_available) return;
        DateTime dt((uint32_t)t);
        switch (_type) {
            case RtcType::DS3231:  _ds3231.adjust(dt);  break;
            case RtcType::DS1307:  _ds1307.adjust(dt);  break;
            case RtcType::PCF8523: _pcf8523.adjust(dt); break;
        }
        Serial.printf("[RTC] Time set to %lu\n", (unsigned long)t);
    }

    bool isAvailable() const { return _available; }

private:
    RtcType     _type      = RtcType::DS3231;
    bool        _available = false;
    RTC_DS3231  _ds3231;
    RTC_DS1307  _ds1307;
    RTC_PCF8523 _pcf8523;
};
