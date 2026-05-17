#pragma once
#include <Arduino.h>
#include <SD.h>

// FSEQ v2 file parser — no-compression and sparse-range support
class FseqPlayer {
public:
    uint32_t channelCount = 0;
    uint32_t frameCount   = 0;
    uint16_t stepTimeMs   = 50;
    uint32_t currentFrame = 0;
    char     mediaFile[128] = {0};

    bool open(const char *path) {
        close();
        _file = SD.open(path);
        if (!_file) return false;
        if (!_parseHeader()) { close(); return false; }
        return true;
    }

    void close() {
        if (_file) _file.close();
        channelCount = 0;
        frameCount   = 0;
        currentFrame = 0;
        mediaFile[0] = 0;
        _dataOffset  = 0;
        _compressionType = 0;
    }

    bool isOpen() const { return (bool)_file; }
    bool atEnd()  const { return currentFrame >= frameCount; }

    // Read next frame into buf (caller must provide channelCount bytes)
    bool readFrame(uint8_t *buf) {
        if (!_file || atEnd()) return false;
        if (_compressionType != 0) {
            Serial.println("[FSEQ] Compressed sequences not supported");
            return false;
        }
        size_t got = _file.read(buf, channelCount);
        if (got != channelCount) return false;
        currentFrame++;
        return true;
    }

    bool seekToFrame(uint32_t frame) {
        if (!_file || frame >= frameCount) return false;
        uint32_t pos = _dataOffset + (uint32_t)frame * channelCount;
        if (!_file.seek(pos)) return false;
        currentFrame = frame;
        return true;
    }

private:
    File     _file;
    uint32_t _dataOffset     = 0;
    uint8_t  _compressionType = 0;

    bool _parseHeader() {
        uint8_t hdr[32];
        if (_file.read(hdr, 32) != 32) return false;

        // Magic "FSEQ"
        if (hdr[0] != 'F' || hdr[1] != 'S' || hdr[2] != 'E' || hdr[3] != 'Q') return false;

        _dataOffset  = hdr[4] | ((uint32_t)hdr[5] << 8);
        uint8_t minV = hdr[6];
        uint8_t majV = hdr[7];
        if (majV != 2) {
            Serial.printf("[FSEQ] Unsupported version %d.%d\n", majV, minV);
            return false;
        }

        channelCount     = hdr[8]  | ((uint32_t)hdr[9]  << 8) | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
        frameCount       = hdr[12] | ((uint32_t)hdr[13] << 8) | ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
        stepTimeMs       = hdr[16] | ((uint16_t)hdr[17] << 8);
        _compressionType = hdr[18];
        // hdr[19] = compression block count, hdr[20] = sparse range count

        Serial.printf("[FSEQ] v%d.%d  ch=%lu  frames=%lu  step=%dms  comp=%d\n",
                      majV, minV, channelCount, frameCount, stepTimeMs, _compressionType);

        // Scan variable data blocks for media filename (type 0x04)
        uint32_t pos = 32;
        while (pos < _dataOffset) {
            uint8_t blk[4];
            if (_file.read(blk, 4) != 4) break;
            uint16_t blkLen  = blk[0] | ((uint16_t)blk[1] << 8);
            uint16_t blkType = blk[2] | ((uint16_t)blk[3] << 8);
            uint16_t dataLen = (blkLen > 4) ? blkLen - 4 : 0;
            if (blkType == 0x0004 && dataLen > 0 && dataLen < (uint16_t)sizeof(mediaFile)) {
                _file.read((uint8_t*)mediaFile, dataLen);
                mediaFile[dataLen] = 0;
                pos += blkLen;
            } else {
                _file.seek(_file.position() + dataLen);
                pos += blkLen;
            }
            if (blkLen == 0) break;
        }

        _file.seek(_dataOffset);
        currentFrame = 0;
        return true;
    }
};
