#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>

// E1.31 sACN sender for one output — maps a channel slice to universes and sends UDP
class E131Sender {
public:
    static constexpr uint16_t E131_PORT = 5568;

    // unicastIp: single destination IP when multicast=false; "" to skip unicast
    // bindIp: bind socket to this local IP to force traffic through a specific interface
    bool begin(uint16_t universeStart, uint16_t universeSize,
               bool multicast, const char *unicastIp,
               const uint8_t mac[6], IPAddress bindIp = INADDR_NONE) {
        _universeStart = universeStart;
        _universeSize  = universeSize;
        _multicast     = multicast;
        _hasUnicast    = (unicastIp && unicastIp[0] != '\0');
        if (_hasUnicast) _unicastIp.fromString(unicastIp);

        memset(_cid, 0, 16);
        memcpy(_cid, mac, 6);
        _cid[6] = 'F'; _cid[7] = 'P'; _cid[8] = 'P';

        if (bindIp != INADDR_NONE)
            return _udp.begin(bindIp, E131_PORT);
        return _udp.begin(E131_PORT);
    }

    // Send a slice of the full FSEQ frame buffer.
    // chanStart: first channel in buf[] to send
    // chanCount: number of channels to send (0 = all from chanStart to end of buf)
    void sendFrame(const uint8_t *buf, uint32_t totalChannels,
                   uint32_t chanStart, uint32_t chanCount) {
        if (chanStart >= totalChannels) return;
        if (chanCount == 0 || chanStart + chanCount > totalChannels)
            chanCount = totalChannels - chanStart;

        const uint8_t *src = buf + chanStart;
        uint32_t ch = 0;
        uint16_t universe = _universeStart;

        while (ch < chanCount) {
            uint32_t chunkSize = chanCount - ch;
            if (chunkSize > _universeSize) chunkSize = _universeSize;

            _buildPacket(universe, src + ch, (uint16_t)chunkSize);

            if (_multicast) {
                IPAddress dest(239, 255, (universe >> 8) & 0xFF, universe & 0xFF);
                _udp.beginPacket(dest, E131_PORT);
                _udp.write(_pkt, _pktLen);
                _udp.endPacket();
            }
            if (_hasUnicast) {
                _udp.beginPacket(_unicastIp, E131_PORT);
                _udp.write(_pkt, _pktLen);
                _udp.endPacket();
            }

            uint16_t idx = universe - _universeStart;
            if (idx < 512) _seqNums[idx]++;
            ch += chunkSize;
            universe++;
        }
    }

private:
    static constexpr int PKT_SIZE = 638;

    WiFiUDP   _udp;
    uint16_t  _universeStart = 1;
    uint16_t  _universeSize  = 512;
    bool      _multicast     = true;
    bool      _hasUnicast    = false;
    IPAddress _unicastIp;
    uint8_t   _cid[16];
    uint8_t   _pkt[PKT_SIZE];
    uint16_t  _pktLen = 0;
    uint8_t   _seqNums[512] = {0};

    void _buildPacket(uint16_t universe, const uint8_t *data, uint16_t len) {
        uint8_t seqNum   = _seqNums[min((uint16_t)(universe - _universeStart), (uint16_t)511)];
        uint16_t propCount = len + 1;
        uint16_t dmpLen   = 11 + propCount;
        uint16_t frameLen = 77 + propCount;
        uint16_t rootLen  = 97 + propCount;
        uint16_t pktLen   = 126 + propCount;
        _pktLen = pktLen;

        memset(_pkt, 0, pktLen);
        _w16(_pkt,  0, 0x0010);
        _w16(_pkt,  2, 0x0000);
        memcpy(_pkt + 4, "\x41\x53\x43\x2d\x45\x31\x2e\x31\x37\x00\x00\x00", 12);
        _w16(_pkt, 16, 0x7000 | rootLen);
        _w32(_pkt, 18, 0x00000004);
        memcpy(_pkt + 22, _cid, 16);
        _w16(_pkt, 38, 0x7000 | frameLen);
        _w32(_pkt, 40, 0x00000002);
        memcpy(_pkt + 44, "SB-Player       ", 16);
        _pkt[76] = 100;
        _pkt[79] = seqNum;
        _w16(_pkt, 81, universe);
        _w16(_pkt, 83, 0x7000 | dmpLen);
        _pkt[85] = 0xa1;
        _pkt[86] = 0xa1;
        _w16(_pkt, 87, 0x0000);
        _w16(_pkt, 89, 0x0001);
        _w16(_pkt, 91, propCount);
        _pkt[93] = 0;
        memcpy(_pkt + 94, data, len);
    }

    void _w16(uint8_t *p, int off, uint16_t v) { p[off]=(v>>8)&0xFF; p[off+1]=v&0xFF; }
    void _w32(uint8_t *p, int off, uint32_t v) {
        p[off]=(v>>24)&0xFF; p[off+1]=(v>>16)&0xFF; p[off+2]=(v>>8)&0xFF; p[off+3]=v&0xFF;
    }
};
