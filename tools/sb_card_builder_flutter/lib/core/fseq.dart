/// FSEQ reader and converter. Layout follows xLights FSEQFile.cpp and FPP's
/// docs/FSEQ_Sequence_File_Format.txt. Output is always plain uncompressed,
/// non-sparse FSEQ v2, which is the only thing the player firmware reads.
library;

import 'dart:convert';
import 'dart:typed_data';

import 'package:archive/archive.dart';

const appName = 'SB Card Builder';
const maxFileSize =
    0xFFFFFFFF; // FAT32 limit; the player also seeks with 32-bit offsets

abstract class ByteSource {
  Future<int> length();
  Future<Uint8List> read(int start, int end);
}

abstract class ByteSink {
  Future<void> add(Uint8List data);
}

class FseqException implements Exception {
  FseqException(this.message);
  final String message;
  @override
  String toString() => message;
}

class CancelledException implements Exception {}

class FseqInfo {
  FseqInfo(
    this.major,
    this.minor,
    this.dataOffset,
    this.channels,
    this.frames,
    this.stepMs,
    this.comp,
    this.blocks,
    this.sparse,
    this.media,
    this.uid,
  );

  final int major, minor, dataOffset;
  final int channels; // bytes stored per frame
  final int frames, stepMs;
  final int comp; // 0 none, 1 zstd, 2 zlib
  final List<(int firstFrame, int length)> blocks;
  final List<(int start, int count)> sparse;
  final String media;
  final Uint8List uid;

  int get fullChannels {
    if (sparse.isEmpty) return channels;
    var max = 0;
    for (final (s, n) in sparse) {
      if (s + n > max) max = s + n;
    }
    return max;
  }

  String describe() {
    final c =
        const {0: 'uncompressed', 1: 'zstd', 2: 'zlib'}[comp] ?? 'comp $comp';
    final sp = sparse.isEmpty ? '' : ', ${sparse.length} sparse ranges';
    final secs = (frames * stepMs / 1000).round();
    return 'v$major.$minor $c$sp, $fullChannels ch, $frames frames @ $stepMs ms ($secs s), '
        'media: ${media.isEmpty ? '-' : media}';
  }
}

/// File name part of a path that may use either separator.
String baseName(String path) {
  final i = path.lastIndexOf(RegExp(r'[\\/]'));
  return i < 0 ? path : path.substring(i + 1);
}

List<(int, int)> _sparse(Uint8List raw) {
  int u24(int i) => raw[i] | raw[i + 1] << 8 | raw[i + 2] << 16;
  return [for (var i = 0; i + 6 <= raw.length; i += 6) (u24(i), u24(i + 3))];
}

Future<FseqInfo> readFseqInfo(ByteSource src) async {
  final size = await src.length();
  if (size < 28) throw FseqException('not an FSEQ file');
  final peek = await src.read(0, 8);
  if ((peek[0] != 0x50 && peek[0] != 0x46) ||
      peek[1] != 0x53 ||
      peek[2] != 0x45 ||
      peek[3] != 0x51) {
    throw FseqException('not an FSEQ file');
  }
  final dataOffset = peek[4] | peek[5] << 8;
  if (dataOffset < 28 || dataOffset > size) {
    throw FseqException('corrupt FSEQ header');
  }
  final hdr = await src.read(0, dataOffset);
  final bd = ByteData.sublistView(hdr);
  final minor = hdr[6], major = hdr[7];
  final varOffset = bd.getUint16(8, Endian.little);
  final channels = bd.getUint32(10, Endian.little);
  final frames = bd.getUint32(14, Endian.little);
  final step = hdr[18] == 0 ? 50 : hdr[18];
  var comp = 0;
  var blocks = <(int, int)>[], sparse = <(int, int)>[];
  var uid = Uint8List(8);
  if (major == 2) {
    if (dataOffset < 32) throw FseqException('corrupt FSEQ header');
    comp = hdr[20] & 0x0F;
    final nblocks = hdr[21] | ((hdr[20] >> 4) << 8);
    final nsparse = hdr[22];
    uid = Uint8List.fromList(hdr.sublist(24, 32));
    var pos = 32;
    for (var i = 0; i < nblocks && pos + 8 <= hdr.length; i++, pos += 8) {
      blocks.add((
        bd.getUint32(pos, Endian.little),
        bd.getUint32(pos + 4, Endian.little),
      ));
    }
    final end = pos + nsparse * 6;
    if (end <= hdr.length) {
      sparse = _sparse(Uint8List.sublistView(hdr, pos, end));
    }
  } else if (major != 1) {
    throw FseqException('unsupported FSEQ version $major.$minor');
  }
  var media = '';
  var pos = varOffset;
  while (pos >= 28 && pos + 4 <= hdr.length) {
    var length = hdr[pos] | hdr[pos + 1] << 8;
    if (length < 4) break;
    var code = String.fromCharCodes(hdr, pos + 2, pos + 4);
    var end = pos + length;
    if (end > hdr.length) end = hdr.length;
    var body = Uint8List.sublistView(hdr, pos + 4, end);
    if (code == 'ED' && pos + 18 <= hdr.length) {
      // extended data: real code, 64-bit file offset, 32-bit length
      code = String.fromCharCodes(hdr, pos + 4, pos + 6);
      final where =
          bd.getUint32(pos + 6, Endian.little) +
          bd.getUint32(pos + 10, Endian.little) * 0x100000000;
      final len = bd.getUint32(pos + 14, Endian.little);
      body = where < size
          ? await src.read(where, (where + len).clamp(0, size))
          : Uint8List(0);
      length = 18;
    }
    if (code == 'mf') {
      final nul = body.indexOf(0);
      media = utf8.decode(
        nul < 0 ? body : Uint8List.sublistView(body, 0, nul),
        allowMalformed: true,
      );
    } else if (code == 'SR') {
      sparse = _sparse(
        body,
      ); // the full table when there are more than 255 ranges
    }
    pos += length;
  }
  return FseqInfo(
    major,
    minor,
    dataOffset,
    channels,
    frames,
    step,
    comp,
    blocks,
    sparse,
    media,
    uid,
  );
}

Uint8List _decompress(int comp, Uint8List data) {
  final List<int> out;
  if (comp == 1) {
    out = zstdCodec.decode(data);
  } else if (comp == 2) {
    out = const ZLibDecoder().decodeBytes(data);
  } else {
    throw FseqException('unknown compression type $comp');
  }
  return out is Uint8List ? out : Uint8List.fromList(out);
}

/// Yields buffers that each hold a whole number of stored frames.
Stream<Uint8List> _frameChunks(ByteSource src, FseqInfo info) async* {
  final n = info.channels;
  if (info.comp == 0) {
    final total = await src.length();
    final perRead = (4 * 1024 * 1024 ~/ n).clamp(1, 1 << 20);
    var pos = info.dataOffset;
    var left = info.frames;
    while (left > 0) {
      final want = left < perRead ? left : perRead;
      final end = (pos + want * n).clamp(0, total);
      final got = (end - pos) ~/ n;
      if (got <= 0) return;
      yield await src.read(pos, pos + got * n);
      pos += got * n;
      left -= got;
      if (got < want) return;
    }
    return;
  }
  var pos = info.dataOffset;
  var carry = Uint8List(0);
  for (final (_, length) in info.blocks) {
    if (length == 0) continue;
    final raw = _decompress(info.comp, await src.read(pos, pos + length));
    pos += length;
    Uint8List buf;
    if (carry.isEmpty) {
      buf = raw;
    } else {
      buf = Uint8List(carry.length + raw.length)
        ..setRange(0, carry.length, carry)
        ..setRange(carry.length, carry.length + raw.length, raw);
    }
    final whole = buf.length - buf.length % n;
    carry = Uint8List.fromList(Uint8List.sublistView(buf, whole));
    if (whole > 0) yield Uint8List.sublistView(buf, 0, whole);
  }
}

class ConvertResult {
  ConvertResult(this.info, this.channels, this.size);
  final FseqInfo info;
  final int channels, size;
}

/// Size and channel count [convertFseq] will produce, for checks before opening the output.
///
/// [maxChannels] channels (0 = all) are kept, starting at channel [first] (0-based).
/// They are renumbered from 1 unless [sparse] is set, which stores them as one sparse
/// range so they keep their xLights numbers. [maxFrame] > 0 is the largest frame the
/// target device can buffer.
({int channels, int first, int dataOffset, int size, Uint8List header})
planConversion(
  FseqInfo info,
  int maxChannels, {
  int first = 0,
  bool sparse = false,
  int maxFrame = 0,
}) {
  if (info.channels == 0 || info.frames == 0) {
    throw FseqException('sequence has no channel data');
  }
  final full = info.fullChannels;
  if (first < 0 || first >= full) {
    throw FseqException(
      'sequence has $full channels - nothing at channel ${first + 1}',
    );
  }
  final avail = full - first;
  final outCh = maxChannels > 0 && maxChannels < avail ? maxChannels : avail;
  if (maxFrame > 0 && outCh > maxFrame) {
    throw FseqException(
      '$outCh channels per frame, but the controller buffers at most $maxFrame',
    );
  }
  final useRange = sparse && first > 0;
  if (useRange && (first >= 1 << 24 || outCh >= 1 << 24)) {
    throw FseqException('channel numbers are too high for a sparse FSEQ range');
  }
  final varOffset = useRange ? 38 : 32; // one 6-byte sparse range before it
  final vars = BytesBuilder();
  for (final (code, text) in [('mf', baseName(info.media)), ('sp', appName)]) {
    if (text.isEmpty) continue;
    final body = [...utf8.encode(text), 0];
    final len = body.length + 4;
    vars.add([len & 0xFF, len >> 8, ...code.codeUnits, ...body]);
  }
  vars.add(Uint8List(-(varOffset + vars.length) % 4));
  final dataOffset = varOffset + vars.length;
  final size = dataOffset + outCh * info.frames;
  if (size > maxFileSize) {
    throw FseqException(
      'result would be ${(size / (1 << 30)).toStringAsFixed(1)} GB; the SD card (FAT32) '
      'and player are limited to 4 GB - shorten the sequence or trim channels',
    );
  }
  final header = Uint8List(dataOffset);
  final bd = ByteData.sublistView(header);
  header.setRange(0, 4, 'PSEQ'.codeUnits);
  bd.setUint16(4, dataOffset, Endian.little);
  header[6] = 0; // minor
  header[7] = 2; // major
  bd.setUint16(8, varOffset, Endian.little); // first variable header
  bd.setUint32(10, outCh, Endian.little);
  bd.setUint32(14, info.frames, Endian.little);
  header[18] = info.stepMs;
  header.setRange(24, 32, info.uid);
  if (useRange) {
    header[22] = 1; // sparse range count
    for (var i = 0; i < 3; i++) {
      header[32 + i] = first >> (8 * i) & 0xFF;
      header[35 + i] = outCh >> (8 * i) & 0xFF;
    }
  }
  header.setRange(varOffset, dataOffset, vars.toBytes());
  return (
    channels: outCh,
    first: first,
    dataOffset: dataOffset,
    size: size,
    header: header,
  );
}

/// Write [src] to [out] as uncompressed FSEQ v2. See [planConversion] for the options.
Future<ConvertResult> convertFseq(
  ByteSource src,
  FseqInfo info,
  ByteSink out, {
  int maxChannels = 0,
  int first = 0,
  bool sparse = false,
  int maxFrame = 0,
  void Function(int done, int total)? onProgress,
  bool Function()? cancelled,
}) async {
  final plan = planConversion(
    info,
    maxChannels,
    first: first,
    sparse: sparse,
    maxFrame: maxFrame,
  );
  final n = info.channels, outCh = plan.channels, full = info.fullChannels;
  final end = first + outCh;
  await out.add(plan.header);
  var done = 0;
  await for (final chunk in _frameChunks(src, info)) {
    if (cancelled != null && cancelled()) throw CancelledException();
    var count = chunk.length ~/ n;
    if (done + count > info.frames) count = info.frames - done;
    if (count <= 0) break;
    if (info.sparse.isEmpty && outCh == n) {
      await out.add(
        count * n == chunk.length
            ? chunk
            : Uint8List.sublistView(chunk, 0, count * n),
      );
    } else {
      final buf = Uint8List(count * outCh);
      for (var f = 0; f < count; f++) {
        final from = f * n, to = f * outCh;
        if (info.sparse.isEmpty) {
          buf.setRange(to, to + outCh, chunk, from + first);
        } else {
          var p = from;
          for (final (start, len) in info.sparse) {
            // copy the part of each stored range that is kept
            final lo = start > first ? start : first;
            final hi = start + len < end ? start + len : end;
            if (lo < hi) {
              buf.setRange(
                to + lo - first,
                to + hi - first,
                chunk,
                p + lo - start,
              );
            }
            p += len;
          }
        }
      }
      await out.add(buf);
    }
    done += count;
    onProgress?.call(done, info.frames);
    if (done >= info.frames) break;
  }
  if (done != info.frames) {
    throw FseqException(
      'file is truncated: $done of ${info.frames} frames found',
    );
  }
  assert(full >= end);
  return ConvertResult(info, outCh, plan.size);
}
