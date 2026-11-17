// FSEQ reader and converter. The format follows xLights FSEQFile.cpp and FPP's
// docs/FSEQ_Sequence_File_Format.txt. Output is always uncompressed FSEQ v2, which is
// the only thing the SB Player and SB Pixel 16 firmware read.
#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

#include <functional>
#include <stdexcept>

class QFile;

constexpr const char* kAppName = "SB Card Builder";
constexpr qint64 kMaxFileSize = 0xFFFFFFFFLL;  // FAT32 limit; the player also seeks with 32-bit offsets

struct FseqError : std::runtime_error {
    explicit FseqError(const QString& message) : std::runtime_error(message.toStdString()) {}
};
struct Cancelled {};

struct FseqInfo {
    int major = 0, minor = 0;
    quint32 dataOffset = 0;
    quint32 channels = 0;  // bytes stored per frame
    quint32 frames = 0;
    int stepMs = 50;
    int comp = 0;          // 0 none, 1 zstd, 2 zlib
    QVector<QPair<quint32, quint32>> blocks;  // (first frame, length)
    QVector<QPair<quint32, quint32>> sparse;  // (start, count)
    QString media;
    QByteArray uid = QByteArray(8, '\0');

    quint32 fullChannels() const;
    QString describe() const;
};

// File name part of a path that may use either separator.
QString baseName(const QString& path);

FseqInfo readFseqInfo(QFile& file);

struct ConvertResult {
    FseqInfo info;
    quint32 channels = 0;
    qint64 size = 0;
};

// Which channels of a sequence to keep, and how they are numbered in the output.
struct Layout {
    qint64 maxChannels = 0;  // channels kept; 0 = all
    qint64 first = 0;        // first channel kept (0-based)
    bool sparse = false;     // store them as one sparse range so they keep their xLights numbers;
                             // otherwise the first kept channel becomes channel 1
    qint64 maxFrame = 0;     // largest frame the target device buffers; 0 = no limit
};

// Write src to dst as uncompressed FSEQ v2. dst is only replaced once the whole file has been written.
ConvertResult convertFseq(const QString& src, const QString& dst, const Layout& layout = {},
                          const std::function<void(quint32 done, quint32 total)>& progress = {},
                          const std::function<bool()>& cancelled = {});
