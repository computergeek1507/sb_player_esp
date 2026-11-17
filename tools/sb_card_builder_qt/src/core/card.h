// Files on the SD card: sequences + audio, playlists, schedule. Limits mirror the firmware.
#pragma once

#include "core/fseq.h"
#include "core/json.h"
#include "core/networks.h"

#include <QMap>
#include <QStringList>

constexpr int kMaxPlaylistEntries = 64;  // MAX_PLAYLIST_ENTRIES in scheduler.h
constexpr int kMaxScheduleEntries = 32;  // MAX_SCHEDULE_ENTRIES
constexpr int kMaxFileName = 100;        // firmware path buffers are 128-140 bytes including "/sequences/"
extern const char* const kDayNames[7];   // bit0 = Sunday

// SB Pixel 16 controller (https://github.com/computergeek1507/sb_pixel_16), limits from its fseq_player.h
constexpr qint64 kPixel16MaxFrame = 262144;  // FSEQ_MAX_FRAME
constexpr int kPixel16MaxFiles = 64;         // FSEQ_MAX_FILES

using LogFn = std::function<void(const QString&)>;

// Layout that gives an SB Pixel 16 just this controller's channels. Its ports are set up by
// start channel. Over E1.31 (and DDP without 'Keep Channel Numbers') channel 1 is the
// controller's first channel, so the file is renumbered to match. DDP with 'Keep Channel
// Numbers' uses xLights' absolute numbers, which a sparse range keeps.
Layout pixel16Layout(const Controller& c);
// Lines to show after exporting for an SB Pixel 16.
QStringList pixel16Notes(const Controller& c, const QString& outDir);

// Convert one sequence into <outDir>/sequences and copy its audio. Throws FseqError / Cancelled.
FseqInfo exportSequence(const QString& src, const QString& outDir, const Layout& layout, bool copyAudio,
                        const QStringList& searchDirs, const LogFn& log,
                        const std::function<void(quint32, quint32)>& progress = {},
                        const std::function<bool()>& cancelled = {});

// Throws std::runtime_error if the file cannot be written.
void writeJson(const QString& path, const Json& doc);
// Empty object if the file is missing or not valid JSON.
Json readJson(const QString& path);

QStringList listCardSequences(const QString& outDir);
QMap<QString, Json> loadPlaylists(const QString& outDir);

// Player windows cannot cross midnight, so one that does is split in two. Times are minutes.
Json scheduleEntries(int days, int start, int end, const QString& playlist);
QString daysText(int mask);
QString safePlaylistName(const QString& name);
