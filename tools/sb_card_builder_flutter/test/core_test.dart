// Core logic tests. The synthetic tests always run. The "real show" tests compare
// against files converted by tools/sb_card_builder.py and only run when the
// environment variables SB_SHOWS (xLights show folders) and SB_REFERENCE
// (folder holding card_<show>/ outputs from the Python tool) are set.
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:sb_card_builder/core/card.dart';
import 'package:sb_card_builder/core/fseq.dart';
import 'package:sb_card_builder/core/networks.dart';

class MemSource implements ByteSource {
  MemSource(this.bytes);
  final Uint8List bytes;
  @override
  Future<int> length() async => bytes.length;
  @override
  Future<Uint8List> read(int start, int end) async =>
      Uint8List.sublistView(bytes, start, end);
}

class MemSink implements ByteSink {
  final builder = BytesBuilder();
  @override
  Future<void> add(Uint8List data) async => builder.add(data);
}

/// Uncompressed v2 file, optionally sparse, with an 'mf' header.
Uint8List makeFseq({
  required int channels,
  required int frames,
  List<(int, int)> sparse = const [],
  String media = '',
}) {
  final vars = BytesBuilder();
  if (media.isNotEmpty) {
    final body = [...utf8.encode(media), 0];
    vars.add([
      (body.length + 4) & 0xFF,
      (body.length + 4) >> 8,
      ...'mf'.codeUnits,
      ...body,
    ]);
  }
  vars.add(Uint8List(-vars.length % 4));
  final varOffset = 32 + sparse.length * 6;
  final dataOffset = varOffset + vars.length;
  final h = Uint8List(dataOffset);
  final bd = ByteData.sublistView(h);
  h.setRange(0, 4, 'PSEQ'.codeUnits);
  bd.setUint16(4, dataOffset, Endian.little);
  h[7] = 2;
  bd.setUint16(8, varOffset, Endian.little);
  bd.setUint32(10, channels, Endian.little);
  bd.setUint32(14, frames, Endian.little);
  h[18] = 25;
  h[22] = sparse.length;
  var p = 32;
  for (final (s, n) in sparse) {
    h.setRange(p, p + 6, [
      s & 0xFF,
      (s >> 8) & 0xFF,
      s >> 16,
      n & 0xFF,
      (n >> 8) & 0xFF,
      n >> 16,
    ]);
    p += 6;
  }
  h.setRange(varOffset, dataOffset, vars.toBytes());
  final data = Uint8List(channels * frames);
  for (var i = 0; i < data.length; i++) {
    data[i] = (i * 7 + i ~/ channels) & 0xFF;
  }
  return Uint8List.fromList([...h, ...data]);
}

void main() {
  test('networks: controllers, old format and output mapping', () {
    final cs = parseNetworks('''<Networks>
      <Controller Name="A" ActiveState="Active" IP="10.0.0.5" Protocol="E131">
        <network ComPort="10.0.0.5" BaudRate="1" NetworkType="E131" MaxChannels="510"/>
        <network ComPort="10.0.0.5" BaudRate="2" NetworkType="E131" MaxChannels="510"/>
        <network ComPort="10.0.0.5" BaudRate="3" NetworkType="E131" MaxChannels="300"/>
        <network ComPort="10.0.0.5" BaudRate="9" NetworkType="E131" MaxChannels="512"/>
      </Controller>
      <Controller Name="B" ActiveState="Inactive" IP="10.0.0.6" Protocol="DDP">
        <network KeepChannelNumbers="0" ComPort="10.0.0.6" NetworkType="DDP" MaxChannels="4800"/>
      </Controller>
      <Controller Name="C" Type="Serial" Protocol="DMX"><network NetworkType="DMX" MaxChannels="512"/></Controller>
      <network ComPort="MULTICAST" BaudRate="20" NetworkType="E131" MaxChannels="512" NumUniverses="2"/>
    </Networks>''');
    expect(cs.map((c) => c.name), ['A', 'B', 'C', 'MULTICAST']);
    expect(cs.map((c) => c.start), [0, 1832, 6632, 7144]);
    expect(cs[3].channels, 1024);
    final r = buildOutputs(cs);
    expect(r.e131Count, 3);
    expect(r.doc['e131'][0], {
      'enabled': true,
      'universe_start': 1,
      'universe_size': 510,
      'channel_start': 0,
      'channel_count': 1320,
      'multicast': false,
      'unicast_ip': '10.0.0.5',
    });
    expect(r.doc['e131'][1]['universe_start'], 9);
    expect(r.doc['e131'][2]['multicast'], true);
    expect(r.doc['ddp'], [
      {
        'enabled': true,
        'ip': '10.0.0.6',
        'channel_start': 1832,
        'channel_count': 4800,
      },
    ]);
    expect(
      r.warnings.length,
      3,
    ); // split universes, keep-channel-numbers, DMX skipped
    expect(r.channelsNeeded, 8168);
  });

  test('fseq: sparse expansion, trimming and media name', () async {
    final src = makeFseq(
      channels: 6,
      frames: 3,
      sparse: [(10, 4), (2, 2)],
      media: r'C:\Shows\Song One.mp3',
    );
    final info = await readFseqInfo(MemSource(src));
    expect(info.fullChannels, 14);
    expect(info.media, r'C:\Shows\Song One.mp3');
    final sink = MemSink();
    final res = await convertFseq(MemSource(src), info, sink, maxChannels: 12);
    final out = sink.builder.toBytes();
    expect(res.channels, 12);
    expect(out.length, res.size);
    final back = await readFseqInfo(MemSource(out));
    expect(
      [
        back.major,
        back.comp,
        back.sparse.length,
        back.channels,
        back.frames,
        back.stepMs,
      ],
      [2, 0, 0, 12, 3, 25],
    );
    expect(back.media, 'Song One.mp3');
    final stored = Uint8List.sublistView(src, info.dataOffset);
    for (var f = 0; f < 3; f++) {
      final frame = Uint8List.sublistView(
        out,
        back.dataOffset + f * 12,
        back.dataOffset + (f + 1) * 12,
      );
      expect(
        frame.sublist(2, 4),
        stored.sublist(f * 6 + 4, f * 6 + 6),
      ); // range (2,2) is stored second
      expect(
        frame.sublist(10, 12),
        stored.sublist(f * 6, f * 6 + 2),
      ); // range (10,4), trimmed to 2
      expect(frame.sublist(0, 2) + frame.sublist(4, 10), List.filled(8, 0));
    }
  });

  test('fseq: rejects junk and truncated files', () async {
    expect(
      () => readFseqInfo(MemSource(Uint8List(64))),
      throwsA(isA<FseqException>()),
    );
    final src = makeFseq(channels: 8, frames: 5);
    final cut = Uint8List.sublistView(src, 0, src.length - 9);
    final info = await readFseqInfo(MemSource(cut));
    expect(
      () => convertFseq(MemSource(cut), info, MemSink()),
      throwsA(isA<FseqException>()),
    );
  });

  test('schedule: midnight split and day text', () {
    expect(scheduleEntries(0x7F, 0, 0, 'a.json'), [
      {
        'days': 127,
        'start_hour': 0,
        'start_min': 0,
        'end_hour': 24,
        'end_min': 0,
        'playlist': 'a.json',
      },
    ]);
    final split = scheduleEntries(125, 17 * 60 + 30, 30, 'a.json');
    expect(split.map((e) => e['days']), [125, 123]);
    expect(split[1]['end_min'], 30);
    expect(daysText(0x41), 'Sun Sat');
    expect(parseTime('7:05'), 425);
    expect(parseTime('24:00'), isNull);
  });

  final shows = Platform.environment['SB_SHOWS'],
      reference = Platform.environment['SB_REFERENCE'];
  final haveReal = shows != null && reference != null;
  for (final show in [
    'hinkstest',
    'cubicle',
    'TestScottPlayer',
    'lights_halloween',
  ]) {
    test(
      'real show $show matches the Python tool',
      () async {
        final ref = Directory('$reference/card_$show');
        final active = parseNetworks(
          await File('$shows/$show/xlights_networks.xml').readAsString(),
        ).where((c) => c.state == 'Active');
        final outputs = buildOutputs(active);
        expect(
          outputs.doc,
          jsonDecode(await File('${ref.path}/outputs.json').readAsString()),
        );
        final files = ref
            .listSync(recursive: true)
            .whereType<File>()
            .where((f) => f.path.endsWith('.fseq'));
        expect(files, isNotEmpty);
        for (final expected in files) {
          final name = baseName(expected.path);
          final src = MemSource(await File('$shows/$show/$name').readAsBytes());
          final info = await readFseqInfo(src);
          final sink = MemSink();
          await convertFseq(
            src,
            info,
            sink,
            maxChannels: outputs.channelsNeeded,
          );
          expect(
            sink.builder.toBytes(),
            await expected.readAsBytes(),
            reason: name,
          );
        }
      },
      skip: haveReal ? false : 'set SB_SHOWS and SB_REFERENCE to run',
      timeout: const Timeout(Duration(minutes: 5)),
    );
  }

  // Formats none of the real shows use (zlib, v1, several zstd frames per block, more
  // than 255 blocks). SB_SYNTH holds the inputs; SB_SYNTH_REF holds the Python tool's
  // conversions of them.
  final synth = Platform.environment['SB_SYNTH'],
      synthRef = Platform.environment['SB_SYNTH_REF'];
  test(
    'synthetic zlib / v1 / multi-frame files match the Python tool',
    () async {
      final files = Directory(synth!)
          .listSync()
          .whereType<File>()
          .where((f) => f.path.endsWith('.fseq'))
          .toList();
      expect(files.length, greaterThanOrEqualTo(4));
      for (final f in files) {
        final name = baseName(f.path);
        final src = MemSource(await f.readAsBytes());
        final sink = MemSink();
        await convertFseq(src, await readFseqInfo(src), sink);
        expect(
          sink.builder.toBytes(),
          await File('$synthRef/sequences/$name').readAsBytes(),
          reason: name,
        );
      }
    },
    skip: synth != null && synthRef != null
        ? false
        : 'set SB_SYNTH and SB_SYNTH_REF to run',
  );
}
