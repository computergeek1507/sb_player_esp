#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>

// FPP MultiSync master — broadcasts sync packets over Ethernet
class MultiSync {
public:
    enum Action : uint8_t {
        ACTION_START = 0,
        ACTION_STOP  = 1,
        ACTION_SYNC  = 2,
        ACTION_OPEN  = 3
    };

    bool begin(uint16_t port, IPAddress bindIp = INADDR_NONE) {
        _port = port;
        if (bindIp != INADDR_NONE)
            return _udp.begin(bindIp, port);
        return _udp.begin(port);
    }

    void sendOpen(const char *filename) {
        _send(ACTION_OPEN, 0, filename);
        _send(ACTION_START, 0, filename);
    }

    void sendSync(uint32_t frameNum, const char *filename) {
        _send(ACTION_SYNC, frameNum, filename);
    }

    void sendStop(const char *filename) {
        _send(ACTION_STOP, 0, filename);
    }

private:
    WiFiUDP  _udp;
    uint16_t _port = 32320;

    void _send(Action action, uint32_t frame, const char *filename) {
        IPAddress broadcast(255, 255, 255, 255);

        uint8_t pkt[128] = {0};
        pkt[0] = 0x01;                          // packet type: sync
        pkt[1] = (uint8_t)action;               // action
        pkt[2] = 0;                             // frame rate divider
        pkt[3] = frame & 0xFF;                  // frame number LE
        pkt[4] = (frame >>  8) & 0xFF;
        pkt[5] = (frame >> 16) & 0xFF;
        pkt[6] = (frame >> 24) & 0xFF;
        if (filename) {
            size_t fnLen = strlen(filename);
            if (fnLen > 120) fnLen = 120;
            memcpy(pkt + 7, filename, fnLen);
        }
        uint16_t pktLen = 7 + (filename ? strlen(filename) + 1 : 1);
        if (pktLen > sizeof(pkt)) pktLen = sizeof(pkt);

        _udp.beginPacket(broadcast, _port);
        _udp.write(pkt, pktLen);
        _udp.endPacket();
    }
};
