#pragma once
#include <Arduino.h>
#include <SD.h>

// FSEQ v2 file parser — uncompressed, non-sparse files only (tools/sb_card_builder.py converts others)
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

        // Layout per xLights FSEQFile.cpp / FPP docs/FSEQ_Sequence_File_Format.txt
        // Magic "PSEQ" (xLights also accepts a leading 'F')
        if ((hdr[0] != 'P' && hdr[0] != 'F') || hdr[1] != 'S' || hdr[2] != 'E' || hdr[3] != 'Q') return false;

        _dataOffset  = hdr[4] | ((uint32_t)hdr[5] << 8);
        uint8_t minV = hdr[6];
        uint8_t majV = hdr[7];
        if (majV != 2) {
            Serial.printf("[FSEQ] Unsupported version %d.%d\n", majV, minV);
            return false;
        }

        uint32_t varOffset = hdr[8] | ((uint32_t)hdr[9] << 8);   // first variable header
        channelCount     = hdr[10] | ((uint32_t)hdr[11] << 8) | ((uint32_t)hdr[12] << 16) | ((uint32_t)hdr[13] << 24);
        frameCount       = hdr[14] | ((uint32_t)hdr[15] << 8) | ((uint32_t)hdr[16] << 16) | ((uint32_t)hdr[17] << 24);
        stepTimeMs       = hdr[18] ? hdr[18] : 50;
        _compressionType = hdr[20] & 0x0F;      // upper nibble + hdr[21] = compression block count
        uint8_t sparseRanges = hdr[22];

        Serial.printf("[FSEQ] v%d.%d  ch=%lu  frames=%lu  step=%dms  comp=%d  sparse=%d\n",
                      majV, minV, channelCount, frameCount, stepTimeMs, _compressionType, sparseRanges);

        if (_compressionType != 0 || sparseRanges != 0) {
            Serial.println("[FSEQ] Compressed / sparse sequences are not supported - convert with tools/sb_card_builder.py");
            return false;
        }

        // Variable headers: 2-byte length (including these 4 bytes), 2-char code, data.
        // 'mf' = media filename.
        uint32_t pos = varOffset;
        while (pos >= 32 && pos + 4 <= _dataOffset) {
            uint8_t blk[4];
            if (!_file.seek(pos) || _file.read(blk, 4) != 4) break;
            uint16_t blkLen = blk[0] | ((uint16_t)blk[1] << 8);
            if (blkLen < 4) break;
            if (blk[2] == 'm' && blk[3] == 'f') {
                size_t n = min((size_t)(blkLen - 4), sizeof(mediaFile) - 1);
                n = _file.read((uint8_t*)mediaFile, n);
                mediaFile[n] = 0;
            }
            pos += blkLen;
        }

        _file.seek(_dataOffset);
        currentFrame = 0;
        return true;
    }
};
