// End-to-end tests of the app state and UI against a real show folder.
// They run only when SB_SHOWS (xLights show folders) and SB_REFERENCE (folder
// holding card_gui/ written by tools/sb_card_builder.py) are set.
// With SB_SHOTS set and --update-goldens, the UI test also saves screenshots there.
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:sb_card_builder/app_state.dart';
import 'package:sb_card_builder/io/fs_io.dart';
import 'package:sb_card_builder/main.dart';

final shows = Platform.environment['SB_SHOWS'];
final reference = Platform.environment['SB_REFERENCE'];
final shots = Platform.environment['SB_SHOTS'];
final Object skip = shows != null && reference != null
    ? false
    : 'set SB_SHOWS and SB_REFERENCE to run';

Future<AppState> buildCard(Directory card) async {
  final s = AppState();
  await s.openShow(IoDir('$shows/hinkstest'));
  await s.openCard(IoDir(card.path));
  expect(await s.writeOutputs(), isNull);
  expect(await s.convertSelected(), isNull);
  expect(s.cardSequences, [
    'TS_MashUp.fseq',
    'bars.fseq',
    'import.fseq',
    'lua.fseq',
  ]);
  expect(
    await s.savePlaylist(
      'xmas',
      ['TS_MashUp.fseq', 'bars.fseq', 'lua.fseq', 'import.fseq'],
      true,
      false,
    ),
    isNull,
  );
  expect(s.addSchedule(125, '17:30', '00:30', 'xmas.json'), isNull);
  expect(s.addSchedule(125, '25:00', '00:30', 'xmas.json'), isNotNull);
  s.setBootPlaylist('xmas.json');
  expect(await s.saveSchedule(), isNull);
  return s;
}

Future<void> loadFonts() async {
  final cache =
      '${File(Platform.resolvedExecutable).parent.parent.parent.path}/material_fonts';
  for (final (family, file) in [
    ('Roboto', 'Roboto-Regular.ttf'),
    ('MaterialIcons', 'MaterialIcons-Regular.otf'),
  ]) {
    final f = File('$cache/$file');
    if (!f.existsSync()) continue;
    final bytes = f.readAsBytesSync();
    await (FontLoader(
      family,
    )..addFont(Future.value(ByteData.sublistView(bytes)))).load();
  }
}

void main() {
  test(
    'builds the same card as the Python tool',
    () async {
      final card = await Directory.systemTemp.createTemp('sbcard');
      addTearDown(() => card.delete(recursive: true));
      final s = await buildCard(card);
      expect(
        s.log.where((l) => l.contains('FAILED') || l.contains('WARNING')),
        isEmpty,
      );
      final ref = '$reference/card_gui';
      for (final name in [
        'outputs.json',
        'schedule.json',
        'playlists/xmas.json',
      ]) {
        expect(
          jsonDecode(File('${card.path}/$name').readAsStringSync()),
          jsonDecode(File('$ref/$name').readAsStringSync()),
          reason: name,
        );
      }
      for (final f in Directory(
        '$ref/sequences',
      ).listSync().whereType<File>()) {
        final name = f.uri.pathSegments.last;
        final mine = File('${card.path}/sequences/$name');
        expect(mine.lengthSync(), f.lengthSync(), reason: name);
        if (name.endsWith('.fseq')) {
          expect(mine.readAsBytesSync(), f.readAsBytesSync(), reason: name);
        }
      }
      expect(
        Directory(card.path)
            .listSync(recursive: true)
            .where((e) => e.path.endsWith('.part')),
        isEmpty,
      );
    },
    skip: skip,
    timeout: const Timeout(Duration(minutes: 5)),
  );

  // SB_PIXEL16 holds show/ (a small show with controllers First, Pix, Rel, Abs) and
  // py_<name>/ folders written by `sb_card_builder.py --pixel16 <name>` from it, plus
  // py_house/ from the lights_halloween show's "House Left" controller.
  final p16 = Platform.environment['SB_PIXEL16'];
  test(
    'SB Pixel 16 export matches the Python tool',
    () async {
      Future<AppState> export(String show, String controller) async {
        final card = await Directory.systemTemp.createTemp('sbp16');
        addTearDown(() => card.delete(recursive: true));
        final s = AppState();
        await s.openShow(IoDir(show));
        await s.openCard(IoDir(card.path));
        s.setTarget(s.controllers.indexWhere((c) => c.name == controller));
        expect(s.target, isNonNegative);
        expect(await s.convertSelected(), isNull);
        return s;
      }

      void same(AppState s, String ref) {
        final mine = Directory('${s.card!.label}/sequences').listSync();
        final theirs = Directory('$ref/sequences').listSync();
        expect(mine.length, theirs.length, reason: ref);
        for (final f in theirs.whereType<File>()) {
          final name = f.uri.pathSegments.last;
          expect(
            File('${s.card!.label}/sequences/$name').readAsBytesSync(),
            f.readAsBytesSync(),
            reason: '$ref $name',
          );
        }
        expect(
          Directory(s.card!.label).listSync().length,
          1,
        ); // only sequences/
      }

      for (final (name, dir) in [
        ('Pix', 'pix'),
        ('Rel', 'rel'),
        ('First', 'first'),
      ]) {
        final s = await export('$p16/show', name);
        expect(s.log.where((l) => l.contains('FAILED')), isEmpty);
        same(s, '$p16/py_$dir');
      }
      // controller beyond the end of the sequences: nothing to export
      final none = await export('$p16/show', 'Abs');
      expect(none.log.where((l) => l.contains('FAILED')).length, 2);
      expect(none.cardSequences, isEmpty);
      if (shows != null) {
        // DDP with Keep Channel Numbers, from a zstd sequence: one sparse range
        final s = await export('$shows/lights_halloween', 'House Left');
        expect(s.log.join('\n'), contains('kept as a sparse range'));
        same(s, '$p16/py_house');
      }
    },
    skip: p16 != null ? false : 'set SB_PIXEL16 to run',
    timeout: const Timeout(Duration(minutes: 5)),
  );

  group('UI', () {
    late Directory card;
    late AppState s;
    setUpAll(() async {
      if (skip != false) return;
      await loadFonts();
      card = await Directory.systemTemp.createTemp('sbcard');
      s = await buildCard(card);
    });
    tearDownAll(() async {
      if (skip == false) await card.delete(recursive: true);
    });

    testWidgets('every tab lays out at desktop and narrow sizes', (
      tester,
    ) async {
      for (final (label, size) in [
        ('wide', const Size(1100, 760)),
        ('narrow', const Size(420, 800)),
      ]) {
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.reset);
        await tester.pumpWidget(
          SbCardBuilderApp(key: ValueKey(label), state: s),
        );
        for (final tab in ['Outputs', 'Sequences', 'Playlists', 'Schedule']) {
          await tester.tap(find.widgetWithText(Tab, tab));
          await tester.pumpAndSettle();
          expect(
            tester.takeException(),
            isNull,
            reason: '$tab tab at $label size',
          );
          if (tab == 'Playlists' && label == 'wide') {
            await tester.tap(find.text('xmas.json').first);
            await tester.pumpAndSettle();
            expect(
              find.text('lua.fseq'),
              findsNWidgets(2),
            ); // on the card, and in the loaded playlist
          }
          if (shots != null && autoUpdateGoldenFiles) {
            await expectLater(
              find.byType(MaterialApp),
              matchesGoldenFile(Uri.file('$shots/${label}_$tab.png')),
            );
          }
        }
      }
    }, skip: skip == false ? null : true);
  });
}
