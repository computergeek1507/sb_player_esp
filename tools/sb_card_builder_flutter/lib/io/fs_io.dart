import 'dart:io';
import 'dart:typed_data';

import 'package:file_selector/file_selector.dart';
import 'package:path/path.dart' as p;

import 'fs_types.dart';

/// Desktop can always pick folders.
bool get folderAccessSupported => true;

Future<FsDir?> pickFolder({
  required String title,
  required bool writable,
}) async {
  final path = await getDirectoryPath(
    confirmButtonText: 'Select',
    initialDirectory: null,
  );
  return path == null ? null : IoDir(path);
}

Future<List<FsFile>> pickFseqFiles() async {
  final files = await openFiles(
    acceptedTypeGroups: const [
      XTypeGroup(label: 'FSEQ sequences', extensions: ['fseq']),
    ],
  );
  return [for (final f in files) IoFile(f.path)];
}

/// A file named by an absolute path (FSEQ media headers hold these). Null if it does not exist.
FsFile? fileAtPath(String path) {
  try {
    return p.isAbsolute(path) && File(path).existsSync() ? IoFile(path) : null;
  } on FileSystemException {
    return null;
  }
}

class IoFile implements FsFile {
  IoFile(this.path);
  final String path;

  @override
  String get name => p.basename(path);
  @override
  FsDir get parent => IoDir(p.dirname(path));
  @override
  Future<int> length() => File(path).length();

  @override
  Future<Uint8List> read(int start, int end) async {
    final raf = await File(path).open();
    try {
      await raf.setPosition(start);
      return await raf.read(end - start);
    } finally {
      await raf.close();
    }
  }
}

class IoDir implements FsDir {
  IoDir(this.path);
  final String path;

  @override
  String get label => path;

  @override
  Future<List<String>> fileNames() async {
    final d = Directory(path);
    if (!await d.exists()) return [];
    return [
      await for (final e in d.list())
        if (e is File) p.basename(e.path),
    ];
  }

  @override
  Future<FsFile?> file(String name) async {
    final f = File(p.join(path, name));
    return await f.exists() ? IoFile(f.path) : null;
  }

  @override
  Future<FsDir?> dir(String name, {bool create = false}) async {
    final d = Directory(p.join(path, name));
    if (create) await d.create(recursive: true);
    return await d.exists() ? IoDir(d.path) : null;
  }

  @override
  Future<FsWriter> create(String name) async => _IoWriter(p.join(path, name));

  @override
  Future<void> delete(String name) => File(p.join(path, name)).delete();
}

class _IoWriter implements FsWriter {
  _IoWriter(this.target)
    : _raf = File('$target.part').openSync(mode: FileMode.write);
  final String target;
  final RandomAccessFile _raf;

  @override
  Future<void> add(Uint8List data) => _raf.writeFrom(data);

  @override
  Future<void> commit() async {
    await _raf.close();
    await File('$target.part').rename(target);
  }

  @override
  Future<void> abort() async {
    try {
      await _raf.close();
      await File('$target.part').delete();
    } on FileSystemException {
      // already gone
    }
  }
}
