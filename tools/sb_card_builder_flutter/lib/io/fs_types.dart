/// Folder access that works the same on desktop (dart:io) and in the browser
/// (File System Access API).
library;

import 'dart:convert';
import 'dart:typed_data';

import '../core/fseq.dart';

abstract class FsFile implements ByteSource {
  String get name;

  /// Folder the file was found in, when known (used to look for audio next to a sequence).
  FsDir? get parent;
}

/// Writes go to a temporary file; nothing replaces the target until [commit].
abstract class FsWriter implements ByteSink {
  Future<void> commit();
  Future<void> abort();
}

abstract class FsDir {
  String get label;
  Future<List<String>> fileNames();
  Future<FsFile?> file(String name);
  Future<FsDir?> dir(String name, {bool create = false});
  Future<FsWriter> create(String name);
  Future<void> delete(String name);
}

Future<String> readText(FsFile f) async =>
    utf8.decode(await f.read(0, await f.length()), allowMalformed: true);

Future<void> writeText(FsDir dir, String name, String text) async {
  final w = await dir.create(name);
  try {
    await w.add(Uint8List.fromList(utf8.encode(text)));
    await w.commit();
  } catch (_) {
    await w.abort();
    rethrow;
  }
}

Future<void> writeJson(FsDir dir, String name, Object doc) =>
    writeText(dir, name, const JsonEncoder.withIndent('  ').convert(doc));

/// Parsed JSON object from [dir]/[name], or null if missing or not valid JSON.
Future<Map<String, dynamic>?> readJson(FsDir dir, String name) async {
  final f = await dir.file(name);
  if (f == null) return null;
  try {
    final doc = jsonDecode(await readText(f));
    return doc is Map<String, dynamic> ? doc : null;
  } on FormatException {
    return null;
  }
}

Future<void> copyFile(FsFile src, FsDir dir, String name) async {
  final total = await src.length();
  final w = await dir.create(name);
  try {
    for (var pos = 0; pos < total; pos += 4 << 20) {
      await w.add(await src.read(pos, (pos + (4 << 20)).clamp(0, total)));
    }
    await w.commit();
  } catch (_) {
    await w.abort();
    rethrow;
  }
}
