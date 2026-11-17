#include "core/card.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

const char* const kDayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// Locate the audio file named in an FSEQ header. Empty if not found.
static QString findMedia(const QString& media, const QStringList& searchDirs) {
    if (media.isEmpty()) return {};
    if (QFileInfo(media).isAbsolute() && QFileInfo(media).isFile()) return media;
    const QString name = baseName(media);
    for (const QString& dir : searchDirs)
        for (const char* sub : {"", "Audio", "audio", "Music", "music", "Media", "media"}) {
            const QString p = QDir(dir).filePath(*sub ? QLatin1String(sub) + '/' + name : name);
            if (QFileInfo(p).isFile()) return p;
        }
    return {};
}

Layout pixel16Layout(const Controller& c) {
    Layout l;
    l.maxChannels = c.channels;
    l.first = c.start;
    l.sparse = c.protocol == QLatin1String("DDP") && c.keepNumbers;
    l.maxFrame = kPixel16MaxFrame;
    return l;
}

QStringList pixel16Notes(const Controller& c, const QString& outDir) {
    QStringList notes;
    const int count = int(listCardSequences(outDir).size());
    if (count > kPixel16MaxFiles)
        notes << QStringLiteral("WARNING the card has %1 sequences; the SB Pixel 16 plays only the first %2")
                     .arg(count).arg(kPixel16MaxFiles);
    if (c.protocol == QLatin1String("DDP") && c.keepNumbers && c.start)
        notes << QStringLiteral("Port start channels on the SB Pixel 16 are xLights channel numbers "
                                "(the controller starts at %1).").arg(c.start + 1);
    else
        notes << QStringLiteral("Port start channels on the SB Pixel 16 count from 1 at the controller's first "
                                "channel, the same as for E1.31.");
    notes << QStringLiteral("Set its input protocol to FSEQ. It loops every file in /sequences; it has no audio, "
                            "playlists or schedule.");
    return notes;
}

FseqInfo exportSequence(const QString& src, const QString& outDir, const Layout& layout, bool copyAudio,
                        const QStringList& searchDirs, const LogFn& log,
                        const std::function<void(quint32, quint32)>& progress,
                        const std::function<bool()>& cancelled) {
    const QString name = QFileInfo(src).fileName();
    if (name.size() > kMaxFileName)
        throw FseqError(QStringLiteral("file name is longer than %1 characters - rename it").arg(kMaxFileName));
    const QDir seqDir(QDir(outDir).filePath(QStringLiteral("sequences")));
    if (!QDir().mkpath(seqDir.path())) throw FseqError(QStringLiteral("cannot create %1").arg(seqDir.path()));
    const ConvertResult r = convertFseq(src, seqDir.filePath(name), layout, progress, cancelled);
    log(QStringLiteral("%1: %2").arg(name, r.info.describe()));
    if (layout.first)
        log(QStringLiteral("   xLights channels %1-%2, %3")
                .arg(layout.first + 1).arg(layout.first + r.channels)
                .arg(layout.sparse ? QStringLiteral("kept as a sparse range") : QStringLiteral("renumbered from 1")));
    log(QStringLiteral("   wrote %1 ch, %2 MB (%3 MB/s read rate)")
            .arg(r.channels)
            .arg(r.size / double(1 << 20), 0, 'f', 1)
            .arg(r.channels * 1000.0 / r.info.stepMs / double(1 << 20), 0, 'f', 2));
    if (!copyAudio || r.info.media.isEmpty()) return r.info;
    const QString audio = findMedia(r.info.media, QStringList{QFileInfo(src).absolutePath()} + searchDirs);
    const QString audioName = QFileInfo(audio).fileName();
    if (audio.isEmpty()) {
        log(QStringLiteral("   WARNING audio not found: %1").arg(r.info.media));
    } else if (!audioName.endsWith(QLatin1String(".mp3"), Qt::CaseInsensitive) &&
               !audioName.endsWith(QLatin1String(".wav"), Qt::CaseInsensitive)) {
        log(QStringLiteral("   WARNING the player only plays .mp3 / .wav - not copied: %1").arg(audioName));
    } else {
        const QString dst = seqDir.filePath(audioName);
        if (!QFileInfo(dst).isFile() || QFileInfo(dst).size() != QFileInfo(audio).size()) {
            QFile::remove(dst);
            if (!QFile::copy(audio, dst)) throw FseqError(QStringLiteral("cannot copy %1").arg(audioName));
        }
        log(QStringLiteral("   audio: %1").arg(audioName));
    }
    return r.info;
}

void writeJson(const QString& path, const Json& doc) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QByteArray::fromStdString(doc.dump(2) + '\n')) < 0 || !f.commit())
        throw std::runtime_error(QStringLiteral("cannot write %1: %2").arg(path, f.errorString()).toStdString());
}

Json readJson(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return Json::object();
    const QByteArray data = f.readAll();
    Json doc = Json::parse(data.cbegin(), data.cend(), nullptr, /*allow_exceptions=*/false);
    return doc.is_object() ? doc : Json::object();
}

QStringList listCardSequences(const QString& outDir) {
    return QDir(QDir(outDir).filePath(QStringLiteral("sequences")))
        .entryList({QStringLiteral("*.fseq")}, QDir::Files, QDir::Name);
}

QMap<QString, Json> loadPlaylists(const QString& outDir) {
    QMap<QString, Json> out;
    const QDir dir(QDir(outDir).filePath(QStringLiteral("playlists")));
    for (const QString& name : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name))
        out.insert(name, readJson(dir.filePath(name)));
    return out;
}

Json scheduleEntries(int days, int start, int end, const QString& playlist) {
    Json out = Json::array();
    auto add = [&](int d, int s, int e) {
        Json o = Json::object();
        o["days"] = d;
        o["start_hour"] = s / 60;
        o["start_min"] = s % 60;
        o["end_hour"] = e / 60;
        o["end_min"] = e % 60;
        o["playlist"] = playlist.toStdString();
        out.push_back(std::move(o));
    };
    if (end > start) {
        add(days, start, end);
    } else {
        add(days, start, 24 * 60);
        if (end > 0) add(((days << 1) | (days >> 6)) & 0x7F, 0, end);  // same nights, next morning
    }
    return out;
}

QString daysText(int mask) {
    if ((mask & 0x7F) == 0x7F) return QStringLiteral("Every day");
    QStringList names;
    for (int i = 0; i < 7; i++)
        if (mask >> i & 1) names << QLatin1String(kDayNames[i]);
    return names.join(' ');
}

QString safePlaylistName(const QString& name) {
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|]"));
    return name.trimmed().replace(bad, QStringLiteral("_"));
}

// ── Player update ────────────────────────────────────────────────────────────

QString updateCardName(UpdateKind kind) {
    return kind == UpdateKind::Firmware ? QStringLiteral("firmware.bin") : QStringLiteral("littlefs.bin");
}

QString checkUpdateImage(const QByteArray& head, qint64 size, UpdateKind kind) {
    const auto byte = [&](int i) { return int(static_cast<unsigned char>(head[i])); };
    if (kind == UpdateKind::Firmware) {
        if (head.size() < 24 || byte(0) != kEspImageMagic)
            return QStringLiteral("not an ESP32 application image - use SBPlayer.ino.bin, "
                                  "not the merged .bin or littlefs.bin");
        const int chip = byte(12) | byte(13) << 8;
        return chip == kEsp32P4ChipId
                   ? QString()
                   : QStringLiteral("built for a different chip (chip id %1), not the ESP32-P4").arg(chip);
    }
    if (head.isEmpty() || byte(0) == kEspImageMagic || size % 4096 || !head.contains("littlefs"))
        return QStringLiteral("not a LittleFS image - use the littlefs.bin built from firmware/SBPlayer/data");
    return {};
}

QString copyUpdate(const QString& src, const QString& outDir, UpdateKind kind) {
    const QString name = QFileInfo(src).fileName();
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly))
        throw std::runtime_error(QStringLiteral("cannot open %1").arg(src).toStdString());
    const qint64 size = in.size();
    const QString why = checkUpdateImage(in.peek(kUpdateHeadBytes), size, kind);
    if (!why.isEmpty()) throw std::runtime_error(QStringLiteral("%1: %2").arg(name, why).toStdString());
    QDir().mkpath(outDir);
    QSaveFile out(QDir(outDir).filePath(updateCardName(kind)));
    bool ok = out.open(QIODevice::WriteOnly);
    while (ok && !in.atEnd()) {
        const QByteArray chunk = in.read(4 << 20);
        ok = !chunk.isEmpty() && out.write(chunk) == chunk.size();
    }
    if (!ok || !out.commit())
        throw std::runtime_error(
            QStringLiteral("cannot write %1: %2").arg(out.fileName(), out.errorString()).toStdString());
    return QStringLiteral("Copied %1 to %2 (%3 bytes); the player installs it at its next boot")
        .arg(name, updateCardName(kind))
        .arg(size);
}
