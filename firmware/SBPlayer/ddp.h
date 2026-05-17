#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>

// DDP (Distributed Display Protocol) sender for one output
// Chunks large channel slices into MTU-safe UDP packets (max 1440 bytes payload)
class DDPSender {
public:
    static constexpr uint16_t DDP_PORT    = 4048;
    static constexpr uint16_t MAX_PAYLOAD = 1440;

    // bindIp: bind to this local IP to force traffic through a specific interface
    bool begin(const char *destIp, IPAddress bindIp = INADDR_NONE) {
        _dest.fromString(destIp);
        if (bindIp != INADDR_NONE)
            return _udp.begin(bindIp, DDP_PORT);
        return _udp.begin(DDP_PORT);
    }

    // Send a slice of the full FSEQ frame buffer.
    // DDP byte offset in each packet = chanStart + local offset within slice,
    // so the receiver maps data to the correct position in its output.
    void sendFrame(const uint8_t *buf, uint32_t totalChannels,
                   uint32_t chanStart, uint32_t chanCount) {
        if (chanStart >= totalChannels) return;
        if (chanCount == 0 || chanStart + chanCount > totalChannels)
            chanCount = totalChannels - chanStart;

        const uint8_t *src = buf + chanStart;
        uint32_t sent = 0;

        while (sent < chanCount) {
            uint16_t len  = (uint16_t)min((uint32_t)MAX_PAYLOAD, chanCount - sent);
            bool     push = (sent + len >= chanCount);
            uint32_t ddpOffset = chanStart + sent;   // absolute byte offset for receiver
            _sendChunk(src + sent, ddpOffset, len, push);
            sent += len;
        }
        _seqNum++;
    }

private:
    WiFiUDP   _udp;
    IPAddress _dest;
    uint8_t   _seqNum = 1;

    void _sendChunk(const uint8_t *data, uint32_t offset, uint16_t len, bool push) {
        uint8_t hdr[10];
        hdr[0] = push ? 0x41 : 0x01;
        hdr[1] = _seqNum;
        hdr[2] = 0x00;
        hdr[3] = 0x01;   // RGB
        hdr[4] = 0x00;
        hdr[5] = 0x01;
        hdr[6] = (offset >> 24) & 0xFF;
        hdr[7] = (offset >> 16) & 0xFF;
        hdr[8] = (offset >>  8) & 0xFF;
        hdr[9] =  offset        & 0xFF;

        _udp.beginPacket(_dest, DDP_PORT);
        _udp.write(hdr, sizeof(hdr));
        _udp.write(data, len);
        _udp.endPacket();
    }
};
