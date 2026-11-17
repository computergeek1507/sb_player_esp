import 'package:flutter/foundation.dart';

import 'core/card.dart';
import 'core/fseq.dart';
import 'core/networks.dart';
import 'io/fs.dart';

/// Everything the UI shows, and every action that touches the show or card folders.
class AppState extends ChangeNotifier {
  FsDir? show, card;
  List<Controller> controllers = [];
  List<bool> use = [];
  List<FsFile> sequences = [];
  Set<int> selected = {};
  List<String> cardSequences = [];
  Map<String, Map<String, dynamic>> playlists = {};
  List<Map<String, dynamic>> schedule = [];
  String bootPlaylist = '';
  final List<String> log = [];

  /// Player update images picked on the Player update tab.
  final Map<UpdateKind, FsFile> updates = {};
  bool trim = true, copyAudio = true;

  /// Export target: -1 for the SB Player, or the index of the controller to export
  /// for as an SB Pixel 16.
  int target = -1;
  bool busy = false;
  double progress = 0;
  bool _cancel = false;

  void say(String line) {
    log.add(line);
    notifyListeners();
  }

  OutputsResult get outputs => buildOutputs([
    for (var i = 0; i < controllers.length; i++)
      if (use[i]) controllers[i],
  ]);

  // ── folders ────────────────────────────────────────────────────────────────

  Future<void> openShow(FsDir dir) async {
    show = dir;
    controllers = [];
    final net = await dir.file('xlights_networks.xml');
    if (net == null) {
      say('No xlights_networks.xml in ${dir.label}');
    } else {
      try {
        controllers = parseNetworks(await readText(net));
      } catch (e) {
        say('Could not read xlights_networks.xml: $e');
      }
    }
    use = [for (final c in controllers) c.usable && c.state == 'Active'];
    target = -1;
    await rescan();
  }

  Future<void> rescan() async {
    final dir = show;
    if (dir == null) return;
    final names =
        (await dir.fileNames())
            .where((n) => n.toLowerCase().endsWith('.fseq'))
            .toList()
          ..sort();
    sequences = [for (final n in names) (await dir.file(n))!];
    selected = {for (var i = 0; i < sequences.length; i++) i};
    notifyListeners();
  }

  void select(int index, bool on) {
    on ? selected.add(index) : selected.remove(index);
    notifyListeners();
  }

  void selectAll(bool on) {
    selected = on ? {for (var i = 0; i < sequences.length; i++) i} : {};
    notifyListeners();
  }

  void setTarget(int controllerIndex) {
    target = controllerIndex;
    notifyListeners();
  }

  void setOptions({bool? trim, bool? copyAudio}) {
    this.trim = trim ?? this.trim;
    this.copyAudio = copyAudio ?? this.copyAudio;
    notifyListeners();
  }

  void addSequences(List<FsFile> files) {
    for (final f in files) {
      selected.add(sequences.length);
      sequences.add(f);
    }
    notifyListeners();
  }

  Future<void> openCard(FsDir dir) async {
    card = dir;
    await reloadCard();
    say(
      'Card folder: ${cardSequences.length} sequences, ${playlists.length} playlists, '
      '${schedule.length} schedule entries',
    );
  }

  Future<void> reloadCard() async {
    final dir = card;
    if (dir == null) return;
    final seq = await dir.dir('sequences');
    cardSequences = seq == null
        ? []
        : ((await seq.fileNames())
              .where((n) => n.toLowerCase().endsWith('.fseq'))
              .toList()
            ..sort());
    playlists = {};
    final pl = await dir.dir('playlists');
    if (pl != null) {
      final names =
          (await pl.fileNames())
              .where((n) => n.toLowerCase().endsWith('.json'))
              .toList()
            ..sort();
      for (final n in names) {
        playlists[n] = await readJson(pl, n) ?? {};
      }
    }
    final sched = await readJson(dir, 'schedule.json') ?? {};
    schedule = [
      for (final e in (sched['entries'] as List? ?? []))
        if (e is Map<String, dynamic>) e,
    ];
    final boot = sched['boot_playlist'];
    bootPlaylist = boot is String ? boot : '';
    notifyListeners();
  }

  // ── outputs ────────────────────────────────────────────────────────────────

  void toggle(int index, bool value) {
    use[index] = value;
    notifyListeners();
  }

  /// Returns a message for the user when nothing was written.
  Future<String?> writeOutputs() async {
    final dir = card;
    if (dir == null) return 'Choose the SD card folder first.';
    final o = outputs;
    if (o.e131Count == 0 && o.ddpCount == 0) {
      return 'No usable controllers are selected.';
    }
    await writeJson(dir, 'outputs.json', o.doc);
    say('Wrote outputs.json: ${o.e131Count} E1.31, ${o.ddpCount} DDP outputs');
    for (final w in o.warnings) {
      say('   WARNING $w');
    }
    return null;
  }

  // ── sequences ──────────────────────────────────────────────────────────────

  void cancel() => _cancel = true;

  Future<String?> convertSelected() async {
    final dir = card;
    if (dir == null) return 'Choose the SD card folder first.';
    final files = [for (final i in selected.toList()..sort()) sequences[i]];
    if (files.isEmpty) return 'Select one or more sequences.';
    final pixel16 = target >= 0 && target < controllers.length
        ? controllers[target]
        : null;
    Layout layout;
    if (pixel16 != null) {
      layout = pixel16Layout(pixel16);
    } else {
      final limit = trim ? outputs.channelsNeeded : 0;
      layout = (maxChannels: limit, first: 0, sparse: false, maxFrame: 0);
      if (trim && limit == 0) {
        say('No outputs selected, so channels are not trimmed.');
      }
    }
    busy = true;
    _cancel = false;
    progress = 0;
    notifyListeners();
    var ok = 0;
    try {
      final seqDir = (await dir.dir('sequences', create: true))!;
      for (var n = 0; n < files.length; n++) {
        try {
          await _export(
            files[n],
            seqDir,
            layout,
            copyAudio && pixel16 == null,
            (f) => _setProgress((n + f) / files.length),
          );
          ok++;
        } on CancelledException {
          say('Cancelled.');
          break;
        } catch (e) {
          say('${files[n].name}: FAILED - $e');
        }
        _setProgress((n + 1) / files.length);
      }
    } finally {
      busy = false;
      progress = 0;
      await reloadCard();
      if (pixel16 != null && ok > 0) {
        pixel16Notes(pixel16, cardSequences.length).forEach(say);
      }
      say('Converted $ok of ${files.length} sequences.');
    }
    return null;
  }

  double _shown = 0;
  void _setProgress(double value) {
    progress = value;
    if ((value - _shown).abs() >= 0.005 || value == 0) {
      _shown = value;
      notifyListeners();
    }
  }

  Future<void> _export(
    FsFile src,
    FsDir seqDir,
    Layout layout,
    bool withAudio,
    void Function(double) onFraction,
  ) async {
    if (src.name.length > maxFileName) {
      throw FseqException(
        'file name is longer than $maxFileName characters - rename it',
      );
    }
    final info = await readFseqInfo(src);
    // fails before anything is created on the card
    final plan = planConversion(
      info,
      layout.maxChannels,
      first: layout.first,
      sparse: layout.sparse,
      maxFrame: layout.maxFrame,
    );
    final w = await seqDir.create(src.name);
    try {
      await convertFseq(
        src,
        info,
        w,
        maxChannels: layout.maxChannels,
        first: layout.first,
        sparse: layout.sparse,
        maxFrame: layout.maxFrame,
        onProgress: (d, t) => onFraction(d / t),
        cancelled: () => _cancel,
      );
      await w.commit();
    } catch (_) {
      await w.abort();
      rethrow;
    }
    say('${src.name}: ${info.describe()}');
    if (layout.first > 0) {
      say(
        '   xLights channels ${layout.first + 1}-${layout.first + plan.channels}, '
        '${layout.sparse ? 'kept as a sparse range' : 'renumbered from 1'}',
      );
    }
    final rate = plan.channels * 1000 / info.stepMs / (1 << 20);
    say(
      '   wrote ${plan.channels} ch, ${(plan.size / (1 << 20)).toStringAsFixed(1)} MB '
      '(${rate.toStringAsFixed(2)} MB/s read rate)',
    );
    if (!withAudio || info.media.isEmpty) return;
    final audio = await _findMedia(info.media, src.parent);
    if (audio == null) {
      say('   WARNING audio not found: ${info.media}');
    } else if (!RegExp(
      r'\.(mp3|wav)$',
      caseSensitive: false,
    ).hasMatch(audio.name)) {
      say(
        '   WARNING the player only plays .mp3 / .wav - not copied: ${audio.name}',
      );
    } else {
      final existing = await seqDir.file(audio.name);
      if (existing == null || await existing.length() != await audio.length()) {
        await copyFile(audio, seqDir, audio.name);
      }
      say('   audio: ${audio.name}');
    }
  }

  Future<FsFile?> _findMedia(String media, FsDir? beside) async {
    final direct = fileAtPath(media);
    if (direct != null) return direct;
    final name = baseName(media);
    for (final root in [beside, show]) {
      if (root == null) continue;
      final here = await root.file(name);
      if (here != null) return here;
      for (final sub in const [
        'Audio',
        'audio',
        'Music',
        'music',
        'Media',
        'media',
      ]) {
        final found = await (await root.dir(sub))?.file(name);
        if (found != null) return found;
      }
    }
    return null;
  }

  // ── playlists ──────────────────────────────────────────────────────────────

  Future<String?> savePlaylist(
    String rawName,
    List<String> entries,
    bool repeat,
    bool shuffle,
  ) async {
    final dir = card;
    if (dir == null) return 'Choose the SD card folder first.';
    final name = safePlaylistName(rawName);
    if (name.isEmpty || name.length > 50 || entries.isEmpty) {
      return 'A playlist needs a name (up to 50 characters) and at least one sequence.';
    }
    if (entries.length > maxPlaylistEntries) {
      return 'The player reads at most $maxPlaylistEntries entries per playlist.';
    }
    final doc = {'repeat': repeat, 'shuffle': shuffle, 'entries': entries};
    await writeJson(
      (await dir.dir('playlists', create: true))!,
      '$name.json',
      doc,
    );
    say('Wrote playlists/$name.json (${entries.length} sequences)');
    await reloadCard();
    return null;
  }

  Future<void> deletePlaylist(String file) async {
    final pl = await card?.dir('playlists');
    if (pl == null) return;
    await pl.delete(file);
    say('Deleted playlists/$file');
    await reloadCard();
  }

  // ── schedule ───────────────────────────────────────────────────────────────

  String? addSchedule(int days, String start, String end, String playlist) {
    final s = parseTime(start), e = parseTime(end);
    if (s == null || e == null) {
      return 'Enter times as HH:MM, for example 17:30.';
    }
    if (days == 0 || playlist.isEmpty) {
      return 'Pick at least one day and a playlist (save a playlist first).';
    }
    final entries = scheduleEntries(days, s, e, playlist);
    if (schedule.length + entries.length > maxScheduleEntries) {
      return 'The player reads at most $maxScheduleEntries schedule entries.';
    }
    schedule.addAll(entries);
    notifyListeners();
    return null;
  }

  void removeSchedule(int index) {
    schedule.removeAt(index);
    notifyListeners();
  }

  void setBootPlaylist(String name) {
    bootPlaylist = name;
    notifyListeners();
  }

  Future<String?> saveSchedule() async {
    final dir = card;
    if (dir == null) return 'Choose the SD card folder first.';
    if (bootPlaylist.isNotEmpty && playlists[bootPlaylist]?['repeat'] != true) {
      say(
        '   NOTE $bootPlaylist does not have Repeat ticked, so it will play once and stop',
      );
    }
    await writeJson(dir, 'schedule.json', {
      'boot_playlist': bootPlaylist,
      'entries': schedule,
    });
    say(
      'Wrote schedule.json (${schedule.length} entries, play at boot: '
      '${bootPlaylist.isEmpty ? 'none' : bootPlaylist})',
    );
    return null;
  }

  // ── player update ──────────────────────────────────────────────────────────

  void setUpdate(UpdateKind kind, FsFile? file) {
    file == null ? updates.remove(kind) : updates[kind] = file;
    notifyListeners();
  }

  /// Copies the picked images to the card root, where the player installs them at its
  /// next boot. Returns a message for the user when nothing was written.
  Future<String?> copyUpdates() async {
    final dir = card;
    if (dir == null) return 'Choose the SD card folder first.';
    if (updates.isEmpty) {
      return 'Choose a firmware and/or web pages image first.';
    }
    busy = true;
    notifyListeners();
    try {
      // web pages first, matching the order the player installs them in
      for (final kind in UpdateKind.values.reversed) {
        final src = updates[kind];
        if (src == null) continue;
        final size = await src.length();
        final why = checkUpdateImage(
          await src.read(0, size < updateHeadBytes ? size : updateHeadBytes),
          size,
          kind,
        );
        if (why != null) {
          say('FAILED - ${src.name}: $why');
          continue;
        }
        await copyFile(src, dir, kind.cardName);
        say(
          'Copied ${src.name} to ${kind.cardName} ($size bytes); '
          'the player installs it at its next boot',
        );
      }
    } finally {
      busy = false;
      notifyListeners();
    }
    return null;
  }
}
