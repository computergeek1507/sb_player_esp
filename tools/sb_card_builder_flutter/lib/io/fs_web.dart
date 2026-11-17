import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:typed_data';

import 'package:web/web.dart' as web;

import 'fs_types.dart';

/// Only Chromium browsers (Chrome, Edge) can open a folder for reading and writing.
bool get folderAccessSupported =>
    (web.window as JSObject).has('showDirectoryPicker');

Future<FsDir?> pickFolder({
  required String title,
  required bool writable,
}) async {
  final opts = JSObject()
    ..['mode'] = (writable ? 'readwrite' : 'read').toJS
    ..['id'] = (writable ? 'sbcard' : 'sbshow').toJS;
  try {
    final h = await (web.window as JSObject)
        .callMethod<JSPromise<web.FileSystemDirectoryHandle>>(
          'showDirectoryPicker'.toJS,
          opts,
        )
        .toDart;
    return WebDir(h);
  } catch (_) {
    return null; // the user cancelled, or the browser refused
  }
}

Future<List<FsFile>> pickFseqFiles() async {
  final type = JSObject()
    ..['description'] = 'FSEQ sequences'.toJS
    ..['accept'] = (JSObject()
      ..['application/octet-stream'] = ['.fseq'.toJS].toJS);
  final opts = JSObject()
    ..['multiple'] = true.toJS
    ..['types'] = [type].toJS;
  try {
    final handles = await (web.window as JSObject)
        .callMethod<JSPromise<JSArray<web.FileSystemFileHandle>>>(
          'showOpenFilePicker'.toJS,
          opts,
        )
        .toDart;
    return [for (final h in handles.toDart) WebFile(h, null)];
  } catch (_) {
    return [];
  }
}

/// Browsers cannot open a file by path.
FsFile? fileAtPath(String path) => null;

class WebFile implements FsFile {
  WebFile(this.handle, this.parent);
  final web.FileSystemFileHandle handle;
  @override
  final FsDir? parent;
  web.File? _file;

  Future<web.File> get _f async => _file ??= await handle.getFile().toDart;

  @override
  String get name => handle.name;
  @override
  Future<int> length() async => (await _f).size;

  @override
  Future<Uint8List> read(int start, int end) async {
    final buf = await (await _f).slice(start, end).arrayBuffer().toDart;
    return buf.toDart.asUint8List();
  }
}

class WebDir implements FsDir {
  WebDir(this.handle);
  final web.FileSystemDirectoryHandle handle;

  @override
  String get label => handle.name;

  @override
  Future<List<String>> fileNames() async {
    final names = <String>[];
    final it = (handle as JSObject).callMethod<JSObject>('values'.toJS);
    while (true) {
      final r = await it.callMethod<JSPromise<JSObject>>('next'.toJS).toDart;
      if ((r['done'] as JSBoolean).toDart) break;
      final entry = r['value'] as web.FileSystemHandle;
      if (entry.kind == 'file') names.add(entry.name);
    }
    return names;
  }

  @override
  Future<FsFile?> file(String name) async {
    try {
      return WebFile(await handle.getFileHandle(name).toDart, this);
    } catch (_) {
      return null;
    }
  }

  @override
  Future<FsDir?> dir(String name, {bool create = false}) async {
    try {
      return WebDir(
        await handle
            .getDirectoryHandle(
              name,
              web.FileSystemGetDirectoryOptions(create: create),
            )
            .toDart,
      );
    } catch (_) {
      return null;
    }
  }

  @override
  Future<FsWriter> create(String name) async {
    final h = await handle
        .getFileHandle(name, web.FileSystemGetFileOptions(create: true))
        .toDart;
    return _WebWriter(await h.createWritable().toDart);
  }

  @override
  Future<void> delete(String name) => handle.removeEntry(name).toDart;
}

/// The browser writes to a swap file and only replaces the target on close().
class _WebWriter implements FsWriter {
  _WebWriter(this.stream);
  final web.FileSystemWritableFileStream stream;

  @override
  Future<void> add(Uint8List data) => stream.write(data.toJS).toDart;
  @override
  Future<void> commit() => stream.close().toDart;
  @override
  Future<void> abort() async {
    try {
      await stream.abort().toDart;
    } catch (_) {
      // already closed
    }
  }
}
