/// xLights `xlights_networks.xml` -> the player's `outputs.json`.
library;

import 'package:xml/xml.dart';

const maxE131Outputs = 8; // MAX_E131_OUTPUTS in firmware config.h
const maxDdpOutputs = 8; // MAX_DDP_OUTPUTS

class Controller {
  Controller(this.name, this.protocol, this.ip, this.state);

  final String name;
  final String protocol;
  final String ip;
  final String state; // Active / Inactive / xLights Only
  int start = 0; // 0-based absolute start channel
  int channels = 0;
  final List<(int universe, int size)> universes = [];
  bool keepNumbers = true; // DDP "Keep Channel Numbers"

  bool get usable => protocol == 'E131' || protocol == 'DDP';

  String get universeText {
    if (protocol != 'E131' || universes.isEmpty) return '';
    final first = universes.first.$1, last = universes.last.$1;
    return universes.length > 1 ? '$first-$last' : '$first';
  }
}

int _int(XmlElement el, String attr, int fallback) =>
    int.tryParse(el.getAttribute(attr) ?? '') ?? fallback;

void _addNetwork(Controller c, XmlElement net) {
  final size = _int(net, 'MaxChannels', 512);
  var count = 1;
  if (net.getAttribute('NetworkType') == 'E131') {
    count = _int(net, 'NumUniverses', 1);
    if (count < 1) count = 1;
  }
  final first = _int(net, 'BaudRate', 1); // xLights stores the universe here
  for (var i = 0; i < count; i++) {
    c.universes.add((first + i, size));
    c.channels += size;
  }
  if (net.getAttribute('KeepChannelNumbers') == '0') c.keepNumbers = false;
}

/// Controllers in channel order. Every controller takes channel space, active or not.
List<Controller> parseNetworks(String xmlText) {
  final root = XmlDocument.parse(xmlText).rootElement;
  final out = <Controller>[];
  String? prevKey;
  for (final el in root.childElements) {
    if (el.name.local == 'Controller') {
      final nets = el.findElements('network').toList();
      String? pick(String? a, String? b, String c) =>
          (a != null && a.isNotEmpty)
          ? a
          : ((b != null && b.isNotEmpty) ? b : c);
      final proto = pick(
        el.getAttribute('Protocol'),
        nets.isEmpty ? null : nets.first.getAttribute('NetworkType'),
        el.getAttribute('Type') ?? '?',
      )!;
      final ip = pick(
        el.getAttribute('IP'),
        nets.isEmpty ? null : nets.first.getAttribute('ComPort'),
        '',
      )!;
      final c = Controller(
        el.getAttribute('Name') ?? '?',
        proto,
        ip,
        el.getAttribute('ActiveState') ?? 'Active',
      );
      for (final n in nets) {
        _addNetwork(c, n);
      }
      out.add(c);
      prevKey = null;
    } else if (el.name.local == 'network') {
      // pre-2020 files: bare outputs, group runs that go to the same device
      final enabled = el.getAttribute('Enabled') ?? 'Yes';
      final key =
          '${el.getAttribute('NetworkType')}|${el.getAttribute('ComPort')}|$enabled';
      if (key != prevKey) {
        final desc = el.getAttribute('Description') ?? '';
        out.add(
          Controller(
            desc.isNotEmpty ? desc : (el.getAttribute('ComPort') ?? '?'),
            el.getAttribute('NetworkType') ?? '?',
            el.getAttribute('ComPort') ?? '',
            enabled != 'No' ? 'Active' : 'Inactive',
          ),
        );
        prevKey = key;
      }
      _addNetwork(out.last, el);
    }
  }
  var ch = 0;
  for (final c in out) {
    c.start = ch;
    ch += c.channels;
  }
  return out;
}

class OutputsResult {
  OutputsResult(this.doc, this.warnings, this.channelsNeeded);
  final Map<String, dynamic> doc;
  final List<String> warnings;
  final int channelsNeeded;

  int get e131Count => (doc['e131'] as List).length;
  int get ddpCount => (doc['ddp'] as List).length;
}

final _ipv4 = RegExp(r'^(\d{1,3}\.){3}\d{1,3}$');

class _Run {
  _Run(this.universe, this.size, this.start)
    : count = size,
      next = universe + 1;
  final int universe, size, start;
  int count, next;
  bool short = false;
}

OutputsResult buildOutputs(Iterable<Controller> controllers) {
  final e131 = <Map<String, dynamic>>[],
      ddp = <Map<String, dynamic>>[],
      warn = <String>[];
  for (final c in controllers) {
    if (!c.usable) {
      warn.add(
        '${c.name}: ${c.protocol} is not supported by the player - skipped',
      );
      continue;
    }
    final multicast = c.ip.toUpperCase() == 'MULTICAST';
    if (!multicast && !_ipv4.hasMatch(c.ip)) {
      warn.add(
        "${c.name}: address '${c.ip}' is not an IPv4 address (host names are not supported) - skipped",
      );
      continue;
    }
    if (c.protocol == 'DDP') {
      if (multicast) {
        warn.add('${c.name}: DDP cannot be multicast - skipped');
        continue;
      }
      if (!c.keepNumbers && c.start != 0) {
        warn.add(
          "${c.name}: 'Keep Channel Numbers' is off in xLights, but the player always sends "
          'absolute channel offsets - this controller will show the wrong data',
        );
      }
      ddp.add({
        'enabled': true,
        'ip': c.ip,
        'channel_start': c.start,
        'channel_count': c.channels,
      });
      continue;
    }
    // E1.31: one player output = consecutive universes of one size (the last may be shorter)
    final runs = <_Run>[];
    var ch = c.start;
    for (final (uni, size) in c.universes) {
      final r = runs.isEmpty ? null : runs.last;
      if (r != null && uni == r.next && !r.short && size <= r.size) {
        r.count += size;
        r.next++;
        r.short = size < r.size;
      } else {
        runs.add(_Run(uni, size, ch));
      }
      ch += size;
    }
    if (runs.length > 1) {
      warn.add(
        '${c.name}: universes are not consecutive / equal sized - uses ${runs.length} player outputs',
      );
    }
    for (final r in runs) {
      e131.add({
        'enabled': true,
        'universe_start': r.universe,
        'universe_size': r.size,
        'channel_start': r.start,
        'channel_count': r.count,
        'multicast': multicast,
        'unicast_ip': multicast ? '' : c.ip,
      });
    }
  }
  void cap(String kind, List<Map<String, dynamic>> list, int max) {
    if (list.length > max) {
      warn.add(
        '${list.length} $kind outputs needed but the player supports $max - the last '
        '${list.length - max} were dropped',
      );
      list.removeRange(max, list.length);
    }
  }

  cap('E1.31', e131, maxE131Outputs);
  cap('DDP', ddp, maxDdpOutputs);
  var needed = 0;
  for (final o in [...e131, ...ddp]) {
    final end = (o['channel_start'] as int) + (o['channel_count'] as int);
    if (end > needed) needed = end;
  }
  return OutputsResult({'e131': e131, 'ddp': ddp}, warn, needed);
}
