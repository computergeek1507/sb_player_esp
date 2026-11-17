/// Playlist and schedule rules shared with the player firmware (scheduler.h), and
/// the export rules for the SB Pixel 16 controller.
library;

import 'dart:convert';
import 'dart:typed_data';

import 'networks.dart';

// SB Pixel 16 (https://github.com/computergeek1507/sb_pixel_16), limits from its fseq_player.h
const pixel16MaxFrame = 262144; // FSEQ_MAX_FRAME
const pixel16MaxFiles = 64; // FSEQ_MAX_FILES

/// Which channels of a sequence to keep; see planConversion in fseq.dart.
typedef Layout = ({int maxChannels, int first, bool sparse, int maxFrame});

/// Layout that gives an SB Pixel 16 just this controller's channels. Its ports are set
/// up by start channel. Over E1.31 (and DDP without 'Keep Channel Numbers') channel 1 is
/// the controller's first channel, so the file is renumbered to match. DDP with 'Keep
/// Channel Numbers' uses xLights' absolute numbers, which a sparse range keeps.
Layout pixel16Layout(Controller c) => (
  maxChannels: c.channels,
  first: c.start,
  sparse: c.protocol == 'DDP' && c.keepNumbers,
  maxFrame: pixel16MaxFrame,
);

/// Lines to show after exporting for an SB Pixel 16.
List<String> pixel16Notes(Controller c, int sequencesOnCard) => [
  if (sequencesOnCard > pixel16MaxFiles)
    'WARNING the card has $sequencesOnCard sequences; the SB Pixel 16 plays only the first $pixel16MaxFiles',
  if (c.protocol == 'DDP' && c.keepNumbers && c.start > 0)
    'Port start channels on the SB Pixel 16 are xLights channel numbers '
        '(the controller starts at ${c.start + 1}).'
  else
    "Port start channels on the SB Pixel 16 count from 1 at the controller's first channel, "
        'the same as for E1.31.',
  'Set its input protocol to FSEQ. It loops every file in /sequences; it has no audio, '
      'playlists or schedule.',
];

const maxPlaylistEntries = 64; // MAX_PLAYLIST_ENTRIES
const maxScheduleEntries = 32; // MAX_SCHEDULE_ENTRIES
const maxFileName =
    100; // firmware path buffers are 128-140 bytes including "/sequences/"
const dayNames = [
  'Sun',
  'Mon',
  'Tue',
  'Wed',
  'Thu',
  'Fri',
  'Sat',
]; // bit0 = Sunday

/// Player windows cannot cross midnight, so split one that does. Times are minutes.
List<Map<String, dynamic>> scheduleEntries(
  int days,
  int start,
  int end,
  String playlist,
) {
  Map<String, dynamic> entry(int d, int s, int e) => {
    'days': d,
    'start_hour': s ~/ 60,
    'start_min': s % 60,
    'end_hour': e ~/ 60,
    'end_min': e % 60,
    'playlist': playlist,
  };
  if (end > start) return [entry(days, start, end)];
  return [
    entry(days, start, 24 * 60),
    if (end > 0)
      entry(
        ((days << 1) | (days >> 6)) & 0x7F,
        0,
        end,
      ), // same nights, next morning
  ];
}

String daysText(int mask) => (mask & 0x7F) == 0x7F
    ? 'Every day'
    : [
        for (var i = 0; i < 7; i++)
          if ((mask >> i) & 1 == 1) dayNames[i],
      ].join(' ');

String hhmm(dynamic hour, dynamic minute) =>
    '${'${hour ?? 0}'.padLeft(2, '0')}:${'${minute ?? 0}'.padLeft(2, '0')}';

/// Minutes since midnight for "HH:MM", or null if it is not a valid time.
int? parseTime(String text) {
  final m = RegExp(r'^\s*(\d{1,2}):(\d{2})\s*$').firstMatch(text);
  if (m == null) return null;
  final h = int.parse(m[1]!), min = int.parse(m[2]!);
  return h > 23 || min > 59 ? null : h * 60 + min;
}

String safePlaylistName(String name) =>
    name.trim().replaceAll(RegExp(r'[\\/:*?"<>|]'), '_');

// ── Player update (firmware sdupdate.h) ──────────────────────────────────────

enum UpdateKind {
  firmware('firmware.bin', 'Firmware (SBPlayer.ino.bin)'),
  littlefs('littlefs.bin', 'Web pages (littlefs.bin)');

  const UpdateKind(this.cardName, this.label);

  /// Name on the card root (SD_FIRMWARE_FILE / SD_LITTLEFS_FILE).
  final String cardName;
  final String label;
}

const espImageMagic = 0xE9; // first byte of an ESP application image
const esp32p4ChipId = 0x0012; // esp_image_header_t.chip_id, at byte 12
const updateHeadBytes = 8192;

/// Why a file is not a player update of [kind], or null if it looks right.
/// [head] is the first [updateHeadBytes] of the file, [size] its length.
String? checkUpdateImage(Uint8List head, int size, UpdateKind kind) {
  if (kind == UpdateKind.firmware) {
    if (head.length < 24 || head[0] != espImageMagic) {
      return 'not an ESP32 application image - use SBPlayer.ino.bin, '
          'not the merged .bin or littlefs.bin';
    }
    final chip = head[12] | head[13] << 8;
    return chip == esp32p4ChipId
        ? null
        : 'built for a different chip (chip id $chip), not the ESP32-P4';
  }
  if (head.isEmpty ||
      head[0] == espImageMagic ||
      size % 4096 != 0 ||
      !latin1.decode(head).contains('littlefs')) {
    return 'not a LittleFS image - use the littlefs.bin built from firmware/SBPlayer/data';
  }
  return null;
}
