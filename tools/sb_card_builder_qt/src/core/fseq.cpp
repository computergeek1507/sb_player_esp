#include "core/fseq.h"

#include <QFile>
#include <QSaveFile>
#include <QtEndian>

#include <zstd.h>

#include <vector>

static quint32 u16(const QByteArray& b, int at) { return qFromLittleEndian<quint16>(b.constData() + at); }
static quint32 u32(const QByteArray& b, int at) { return qFromLittleEndian<quint32>(b.constData() + at); }

quint32 FseqInfo::fullChannels() const {
    if (sparse.isEmpty()) return channels;
    quint32 top = 0;
    for (const auto& r : sparse) top = qMax(top, r.first + r.second);
    return top;
}

QString FseqInfo::describe() const {
    const QString c = comp == 0 ? QStringLiteral("uncompressed") : comp == 1 ? QStringLiteral("zstd")
                      : comp == 2 ? QStringLiteral("zlib") : QStringLiteral("comp %1").arg(comp);
    const QString sp = sparse.isEmpty() ? QString() : QStringLiteral(", %1 sparse ranges").arg(sparse.size());
    return QStringLiteral("v%1.%2 %3%4, %5 ch, %6 frames @ %7 ms (%8 s), media: %9")
        .arg(major).arg(minor).arg(c, sp).arg(fullChannels()).arg(frames).arg(stepMs)
        .arg(qRound(double(frames) * stepMs / 1000.0)).arg(media.isEmpty() ? QStringLiteral("-") : media);
}

QString baseName(const QString& path) {
    const int i = qMax(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    return i < 0 ? path : path.mid(i + 1);
}

static QVector<QPair<quint32, quint32>> sparseTable(const QByteArray& raw) {
    auto u24 = [&raw](int i) {
        const auto* p = reinterpret_cast<const uchar*>(raw.constData()) + i;
        return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16;
    };
    QVector<QPair<quint32, quint32>> out;
    for (int i = 0; i + 6 <= raw.size(); i += 6) out.append({u24(i), u24(i + 3)});
    return out;
}

FseqInfo readFseqInfo(QFile& f) {
    const qint64 size = f.size();
    f.seek(0);
    const QByteArray peek = f.read(8);
    if (size < 28 || peek.size() < 8 || (peek[0] != 'P' && peek[0] != 'F') || peek[1] != 'S' || peek[2] != 'E' ||
        peek[3] != 'Q')
        throw FseqError(QStringLiteral("not an FSEQ file"));
    FseqInfo info;
    info.dataOffset = u16(peek, 4);
    if (info.dataOffset < 28 || info.dataOffset > size) throw FseqError(QStringLiteral("corrupt FSEQ header"));
    f.seek(0);
    const QByteArray hdr = f.read(info.dataOffset);
    if (hdr.size() != int(info.dataOffset)) throw FseqError(QStringLiteral("corrupt FSEQ header"));
    info.minor = uchar(hdr[6]);
    info.major = uchar(hdr[7]);
    const int varOffset = u16(hdr, 8);
    info.channels = u32(hdr, 10);
    info.frames = u32(hdr, 14);
    info.stepMs = uchar(hdr[18]) ? uchar(hdr[18]) : 50;
    if (info.major == 2) {
        if (info.dataOffset < 32) throw FseqError(QStringLiteral("corrupt FSEQ header"));
        info.comp = uchar(hdr[20]) & 0x0F;
        const int nblocks = uchar(hdr[21]) | ((uchar(hdr[20]) >> 4) << 8);
        const int nsparse = uchar(hdr[22]);
        info.uid = hdr.mid(24, 8);
        int pos = 32;
        for (int i = 0; i < nblocks && pos + 8 <= hdr.size(); i++, pos += 8)
            info.blocks.append({u32(hdr, pos), u32(hdr, pos + 4)});
        if (pos + nsparse * 6 <= hdr.size()) info.sparse = sparseTable(hdr.mid(pos, nsparse * 6));
    } else if (info.major != 1) {
        throw FseqError(QStringLiteral("unsupported FSEQ version %1.%2").arg(info.major).arg(info.minor));
    }
    int pos = varOffset;
    while (pos >= 28 && pos + 4 <= hdr.size()) {
        int length = u16(hdr, pos);
        if (length < 4) break;
        QByteArray code = hdr.mid(pos + 2, 2);
        QByteArray body = hdr.mid(pos + 4, length - 4);
        if (code == "ED" && pos + 18 <= hdr.size()) {
            // extended data: real code, 64-bit file offset, 32-bit length
            code = hdr.mid(pos + 4, 2);
            const qint64 where = qFromLittleEndian<qint64>(hdr.constData() + pos + 6);
            const quint32 len = u32(hdr, pos + 14);
            body.clear();
            if (where >= 0 && where < size && f.seek(where)) body = f.read(len);
            length = 18;
        }
        if (code == "mf") {
            const int nul = body.indexOf('\0');
            info.media = QString::fromUtf8(nul < 0 ? body : body.left(nul));
        } else if (code == "SR") {
            info.sparse = sparseTable(body);  // the full table when there are more than 255 ranges
        }
        pos += length;
    }
    return info;
}

static QByteArray zstdDecompress(const QByteArray& in) {
    struct Stream {
        ZSTD_DStream* d = ZSTD_createDStream();
        ~Stream() { ZSTD_freeDStream(d); }
    } s;
    if (!s.d) throw FseqError(QStringLiteral("zstd: out of memory"));
    QByteArray out;
    std::vector<char> buf(ZSTD_DStreamOutSize());
    ZSTD_inBuffer ib{in.constData(), size_t(in.size()), 0};
    bool outputFull = false;
    while (ib.pos < ib.size || outputFull) {  // a block can hold several zstd frames
        ZSTD_outBuffer ob{buf.data(), buf.size(), 0};
        const size_t r = ZSTD_decompressStream(s.d, &ob, &ib);
        if (ZSTD_isError(r))
            throw FseqError(QStringLiteral("zstd: %1").arg(QLatin1String(ZSTD_getErrorName(r))));
        out.append(buf.data(), qsizetype(ob.pos));
        outputFull = ob.pos == ob.size;
    }
    return out;
}

static QByteArray decompress(int comp, const QByteArray& data, qint64 sizeHint) {
    if (comp == 1) return zstdDecompress(data);
    if (comp == 2) {
        // qUncompress wants a big-endian size before the zlib stream; it grows the buffer if the hint is low
        QByteArray framed(4, '\0');
        qToBigEndian<quint32>(quint32(qBound<qint64>(qint64(1024), sizeHint, qint64(0x7FFFFFFF))), framed.data());
        framed += data;
        const QByteArray out = qUncompress(framed);
        if (out.isEmpty() && !data.isEmpty()) throw FseqError(QStringLiteral("zlib: corrupt data"));
        return out;
    }
    throw FseqError(QStringLiteral("unknown compression type %1").arg(comp));
}

ConvertResult convertFseq(const QString& src, const QString& dst, const Layout& layout,
                          const std::function<void(quint32, quint32)>& progress,
                          const std::function<bool()>& cancelled) {
    QFile f(src);
    if (!f.open(QIODevice::ReadOnly)) throw FseqError(QStringLiteral("cannot open %1").arg(src));
    const FseqInfo info = readFseqInfo(f);
    if (!info.channels || !info.frames) throw FseqError(QStringLiteral("sequence has no channel data"));
    const quint32 n = info.channels, full = info.fullChannels();
    if (layout.first < 0 || layout.first >= full)
        throw FseqError(QStringLiteral("sequence has %1 channels - nothing at channel %2").arg(full).arg(layout.first + 1));
    const quint32 first = quint32(layout.first), avail = full - first;
    const quint32 outCh = layout.maxChannels > 0 && layout.maxChannels < avail ? quint32(layout.maxChannels) : avail;
    if (layout.maxFrame > 0 && outCh > layout.maxFrame)
        throw FseqError(QStringLiteral("%1 channels per frame, but the controller buffers at most %2")
                            .arg(outCh).arg(layout.maxFrame));
    QByteArray ranges;  // one sparse range: 3-byte start, 3-byte count
    if (layout.sparse && first) {
        if (first >= 1u << 24 || outCh >= 1u << 24)
            throw FseqError(QStringLiteral("channel numbers are too high for a sparse FSEQ range"));
        for (quint32 v : {first, outCh})
            for (int shift : {0, 8, 16}) ranges += char(v >> shift & 0xFF);
    }
    const quint32 varOffset = 32 + ranges.size();

    QByteArray vars;
    const QPair<const char*, QString> headers[] = {{"mf", baseName(info.media)}, {"sp", QLatin1String(kAppName)}};
    for (const auto& h : headers) {
        if (h.second.isEmpty()) continue;
        const QByteArray body = h.second.toUtf8() + '\0';
        const quint16 len = quint16(body.size() + 4);
        vars += char(len & 0xFF);
        vars += char(len >> 8);
        vars += h.first;
        vars += body;
    }
    vars += QByteArray((4 - (varOffset + vars.size()) % 4) % 4, '\0');
    const quint32 dataOffset = varOffset + vars.size();
    const qint64 size = qint64(dataOffset) + qint64(outCh) * info.frames;
    if (size > kMaxFileSize)
        throw FseqError(QStringLiteral("result would be %1 GB; the SD card (FAT32) and player are limited to 4 GB - "
                                       "shorten the sequence or trim channels")
                            .arg(size / double(1LL << 30), 0, 'f', 1));
    QByteArray header(32, '\0');
    memcpy(header.data(), "PSEQ", 4);
    qToLittleEndian<quint16>(quint16(dataOffset), header.data() + 4);
    header[6] = 0;  // minor
    header[7] = 2;  // major
    qToLittleEndian<quint16>(quint16(varOffset), header.data() + 8);  // first variable header
    qToLittleEndian<quint32>(outCh, header.data() + 10);
    qToLittleEndian<quint32>(info.frames, header.data() + 14);
    header[18] = char(info.stepMs);
    header[22] = ranges.isEmpty() ? 0 : 1;  // sparse range count
    memcpy(header.data() + 24, info.uid.constData(), 8);

    QSaveFile out(dst);
    if (!out.open(QIODevice::WriteOnly)) throw FseqError(QStringLiteral("cannot write %1").arg(dst));
    out.write(header + ranges + vars);

    quint32 done = 0;
    QByteArray scratch;
    // Takes a buffer holding whole stored frames and writes them in the output layout.
    auto emitFrames = [&](const char* data, quint32 count) {
        if (cancelled && cancelled()) throw Cancelled{};
        count = qMin(count, info.frames - done);
        if (info.sparse.isEmpty() && outCh == n) {
            out.write(data, qint64(count) * n);
        } else {
            scratch.fill('\0', qsizetype(count) * outCh);
            for (quint32 fr = 0; fr < count; fr++) {
                const char* from = data + qint64(fr) * n;
                char* to = scratch.data() + qint64(fr) * outCh;
                if (info.sparse.isEmpty()) {
                    memcpy(to, from + first, outCh);
                } else {
                    for (const auto& r : info.sparse) {  // copy the part of each stored range that is kept
                        const qint64 lo = qMax<qint64>(r.first, first);
                        const qint64 hi = qMin<qint64>(qint64(r.first) + r.second, qint64(first) + outCh);
                        if (lo < hi) memcpy(to + (lo - first), from + (lo - r.first), size_t(hi - lo));
                        from += r.second;
                    }
                }
            }
            out.write(scratch);
        }
        done += count;
        if (progress) progress(done, info.frames);
    };

    f.seek(info.dataOffset);
    if (info.comp == 0) {
        const quint32 perRead = qMax<quint32>(1, (4u << 20) / n);
        while (done < info.frames) {
            const QByteArray chunk = f.read(qint64(qMin(perRead, info.frames - done)) * n);
            const quint32 got = quint32(chunk.size() / n);
            if (!got) break;
            emitFrames(chunk.constData(), got);
        }
    } else {
        QByteArray carry;
        for (int b = 0; b < info.blocks.size() && done < info.frames; b++) {
            const quint32 length = info.blocks[b].second;
            if (!length) continue;
            const QByteArray raw = f.read(length);
            carry += decompress(info.comp, raw, qint64(raw.size()) * 8);
            const quint32 whole = quint32(carry.size() / n);
            if (whole) {
                emitFrames(carry.constData(), whole);
                carry.remove(0, qsizetype(whole) * n);
            }
        }
    }
    if (done != info.frames)
        throw FseqError(QStringLiteral("file is truncated: %1 of %2 frames found").arg(done).arg(info.frames));
    if (!out.commit()) throw FseqError(QStringLiteral("cannot write %1: %2").arg(dst, out.errorString()));
    return {info, outCh, size};
}
