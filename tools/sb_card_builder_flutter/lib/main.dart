import 'package:flutter/material.dart';

import 'app_state.dart';
import 'core/card.dart';
import 'core/fseq.dart' show appName;
import 'io/fs.dart';

void main() => runApp(SbCardBuilderApp(state: AppState()));

class SbCardBuilderApp extends StatelessWidget {
  const SbCardBuilderApp({super.key, required this.state});
  final AppState state;

  @override
  Widget build(BuildContext context) {
    // Same palette as the player's web UI (firmware/SBPlayer/data/*.html).
    const accent = Color(0xFF44AAFF); // #4af
    const scheme = ColorScheme.dark(
      primary: accent,
      onPrimary: Color(0xFF000000),
      primaryContainer: Color(0xFF1A3A55),
      onPrimaryContainer: accent,
      secondary: accent,
      onSecondary: Color(0xFF000000),
      secondaryContainer: Color(0xFF1A3A55),
      onSecondaryContainer: Color(0xFFEEEEEE),
      tertiary: Color(0xFF44FF44), // #4f4
      onTertiary: Color(0xFF000000),
      error: Color(0xFFFF4444), // #f44
      onError: Color(0xFFFFFFFF),
      errorContainer: Color(0xFF3A1A1A),
      onErrorContainer: Color(0xFFFF4444),
      surface: Color(0xFF111111), // page background
      onSurface: Color(0xFFEEEEEE),
      onSurfaceVariant: Color(0xFF888888),
      surfaceContainerLowest: Color(0xFF111111),
      surfaceContainerLow: Color(0xFF161616),
      surfaceContainer: Color(0xFF1E1E1E), // cards
      surfaceContainerHigh: Color(0xFF222222), // inputs
      surfaceContainerHighest: Color(0xFF333333),
      outline: Color(0xFF444444),
      outlineVariant: Color(0xFF333333),
    );
    return MaterialApp(
      title: appName,
      debugShowCheckedModeBanner: false,
      themeMode: ThemeMode.dark,
      darkTheme: ThemeData(
        colorScheme: scheme,
        scaffoldBackgroundColor: scheme.surface,
        dividerColor: scheme.outlineVariant,
        cardTheme: const CardThemeData(color: Color(0xFF1E1E1E)),
        visualDensity: VisualDensity.compact,
      ),
      home: HomePage(state: state),
    );
  }
}

/// Shows the message an action returned, if any.
void report(BuildContext context, String? message) {
  if (message == null || !context.mounted) return;
  ScaffoldMessenger.of(context)
    ..hideCurrentSnackBar()
    ..showSnackBar(SnackBar(content: Text(message)));
}

/// Runs an action that touches the disk and reports its message or its failure.
Future<void> run(
  BuildContext context,
  AppState state,
  Future<String?> Function() action,
) async {
  String? message;
  try {
    message = await action();
  } catch (e) {
    message = 'Failed: $e';
    state.say(message);
  }
  if (context.mounted) report(context, message);
}

class HomePage extends StatelessWidget {
  const HomePage({super.key, required this.state});
  final AppState state;

  @override
  Widget build(BuildContext context) {
    return DefaultTabController(
      length: 4,
      child: Scaffold(
        appBar: AppBar(
          title: const Text(appName),
          bottom: const TabBar(
            tabs: [
              Tab(text: 'Outputs'),
              Tab(text: 'Sequences'),
              Tab(text: 'Playlists'),
              Tab(text: 'Schedule'),
            ],
          ),
        ),
        body: ListenableBuilder(
          listenable: state,
          builder: (context, _) => Column(
            children: [
              if (!folderAccessSupported)
                MaterialBanner(
                  content: const Text(
                    'This browser cannot open folders. Use Chrome or Edge on a computer, '
                    'or the Windows app.',
                  ),
                  actions: const [SizedBox.shrink()],
                  backgroundColor: Theme.of(context).colorScheme.errorContainer,
                ),
              FolderBar(state: state),
              const Divider(height: 1),
              Expanded(
                child: TabBarView(
                  children: [
                    OutputsTab(state: state),
                    SequencesTab(state: state),
                    PlaylistsTab(state: state),
                    ScheduleTab(state: state),
                  ],
                ),
              ),
              if (state.log.isNotEmpty) ...[
                const Divider(height: 1),
                LogPanel(lines: state.log),
              ],
            ],
          ),
        ),
      ),
    );
  }
}

class FolderBar extends StatelessWidget {
  const FolderBar({super.key, required this.state});
  final AppState state;

  @override
  Widget build(BuildContext context) {
    Widget row(
      String label,
      FsDir? dir,
      bool writable,
      Future<void> Function(FsDir) open,
    ) => Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(
        children: [
          SizedBox(width: 150, child: Text(label)),
          Expanded(
            child: Text(
              dir?.label ?? 'not chosen',
              overflow: TextOverflow.ellipsis,
              style: TextStyle(
                color: dir == null
                    ? Theme.of(context).colorScheme.outline
                    : null,
              ),
            ),
          ),
          OutlinedButton(
            onPressed: state.busy
                ? null
                : () => run(context, state, () async {
                    final picked = await pickFolder(
                      title: label,
                      writable: writable,
                    );
                    if (picked != null) await open(picked);
                    return null;
                  }),
            child: const Text('Browse...'),
          ),
        ],
      ),
    );
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 8),
      child: Column(
        children: [
          row('xLights show folder', state.show, false, state.openShow),
          row('SD card folder', state.card, true, state.openCard),
        ],
      ),
    );
  }
}

class LogPanel extends StatefulWidget {
  const LogPanel({super.key, required this.lines});
  final List<String> lines;

  @override
  State<LogPanel> createState() => _LogPanelState();
}

class _LogPanelState extends State<LogPanel> {
  final _scroll = ScrollController();
  int _seen = 0;

  @override
  void dispose() {
    _scroll.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (widget.lines.length != _seen) {
      _seen = widget.lines.length;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (_scroll.hasClients) {
          _scroll.jumpTo(_scroll.position.maxScrollExtent);
        }
      });
    }
    return Container(
      constraints: const BoxConstraints(maxHeight: 130),
      width: double.infinity,
      color: Theme.of(context).colorScheme.surfaceContainerLow,
      child: SelectionArea(
        child: ListView.builder(
          controller: _scroll,
          shrinkWrap: true,
          padding: const EdgeInsets.all(8),
          itemCount: widget.lines.length,
          itemBuilder: (context, i) => Text(
            widget.lines[i],
            style: TextStyle(
              fontFamily: 'monospace',
              fontSize: 12,
              color:
                  widget.lines[i].contains('WARNING') ||
                      widget.lines[i].contains('FAILED')
                  ? Theme.of(context).colorScheme.error
                  : null,
            ),
          ),
        ),
      ),
    );
  }
}

// ── Outputs ──────────────────────────────────────────────────────────────────

class OutputsTab extends StatelessWidget {
  const OutputsTab({super.key, required this.state});
  final AppState state;

  @override
  Widget build(BuildContext context) {
    if (state.controllers.isEmpty) {
      return const Center(
        child: Text('Choose an xLights show folder to list its controllers.'),
      );
    }
    final o = state.outputs;
    final scheme = Theme.of(context).colorScheme;
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Expanded(
            child: SingleChildScrollView(
              child: SingleChildScrollView(
                scrollDirection: Axis.horizontal,
                child: DataTable(
                  columnSpacing: 28,
                  columns: const [
                    DataColumn(label: Text('Controller')),
                    DataColumn(label: Text('Protocol')),
                    DataColumn(label: Text('Address')),
                    DataColumn(label: Text('xLights state')),
                    DataColumn(label: Text('Start ch'), numeric: true),
                    DataColumn(label: Text('Channels'), numeric: true),
                    DataColumn(label: Text('Universes')),
                  ],
                  rows: [
                    for (final (i, c) in state.controllers.indexed)
                      DataRow(
                        selected: state.use[i],
                        onSelectChanged: (v) => state.toggle(i, v ?? false),
                        cells: [
                          DataCell(Text(c.name)),
                          DataCell(Text(c.protocol)),
                          DataCell(Text(c.ip)),
                          DataCell(Text(c.state)),
                          DataCell(Text('${c.start + 1}')),
                          DataCell(Text('${c.channels}')),
                          DataCell(Text(c.universeText)),
                        ],
                      ),
                  ],
                ),
              ),
            ),
          ),
          const SizedBox(height: 8),
          Text(
            "Only controllers marked 'Active' in xLights are ticked by default.",
            style: Theme.of(context).textTheme.bodySmall,
          ),
          const SizedBox(height: 8),
          if (o.warnings.isEmpty)
            Text(
              '${o.e131Count} E1.31 and ${o.ddpCount} DDP outputs, ${o.channelsNeeded} channels.',
            )
          else
            ConstrainedBox(
              constraints: const BoxConstraints(maxHeight: 90),
              child: SingleChildScrollView(
                child: Text(
                  o.warnings.join('\n'),
                  style: TextStyle(color: scheme.error),
                ),
              ),
            ),
          const SizedBox(height: 8),
          Align(
            alignment: Alignment.centerRight,
            child: FilledButton(
              onPressed: state.busy
                  ? null
                  : () => run(context, state, state.writeOutputs),
              child: const Text('Write outputs.json to card'),
            ),
          ),
        ],
      ),
    );
  }
}

// ── Sequences ────────────────────────────────────────────────────────────────

class SequencesTab extends StatelessWidget {
  const SequencesTab({super.key, required this.state});
  final AppState state;

  @override
  Widget build(BuildContext context) {
    final all =
        state.selected.length == state.sequences.length &&
        state.sequences.isNotEmpty;
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              OutlinedButton(
                onPressed: state.busy
                    ? null
                    : () async => state.addSequences(await pickFseqFiles()),
                child: const Text('Add files...'),
              ),
              OutlinedButton(
                onPressed: state.busy || state.show == null
                    ? null
                    : () => run(context, state, () async {
                        await state.rescan();
                        return null;
                      }),
                child: const Text('Rescan show folder'),
              ),
              TextButton(
                onPressed: state.busy || state.sequences.isEmpty
                    ? null
                    : () => state.selectAll(!all),
                child: Text(all ? 'Select none' : 'Select all'),
              ),
            ],
          ),
          const SizedBox(height: 8),
          Expanded(
            child: state.sequences.isEmpty
                ? const Center(
                    child: Text(
                      'No .fseq files yet. Choose a show folder or add files.',
                    ),
                  )
                : ListView.builder(
                    itemCount: state.sequences.length,
                    itemBuilder: (context, i) => CheckboxListTile(
                      dense: true,
                      controlAffinity: ListTileControlAffinity.leading,
                      value: state.selected.contains(i),
                      title: Text(state.sequences[i].name),
                      subtitle: state.sequences[i].parent == null
                          ? null
                          : Text(state.sequences[i].parent!.label),
                      onChanged: state.busy
                          ? null
                          : (v) => state.select(i, v == true),
                    ),
                  ),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Row(
              children: [
                const Text('Export for'),
                const SizedBox(width: 12),
                Expanded(
                  child: DropdownButton<int>(
                    key: const Key('target'),
                    isExpanded: true,
                    value: state.target,
                    items: [
                      const DropdownMenuItem(
                        value: -1,
                        child: Text('SB Player'),
                      ),
                      for (var i = 0; i < state.controllers.length; i++)
                        if (state.controllers[i].channels > 0)
                          DropdownMenuItem(
                            value: i,
                            child: Text(
                              'SB Pixel 16: ${state.controllers[i].name} '
                              '(its own channels only, no audio)',
                              overflow: TextOverflow.ellipsis,
                            ),
                          ),
                    ],
                    onChanged: state.busy
                        ? null
                        : (v) => state.setTarget(v ?? -1),
                  ),
                ),
              ],
            ),
          ),
          CheckboxListTile(
            dense: true,
            controlAffinity: ListTileControlAffinity.leading,
            value: state.trim,
            title: const Text(
              'Drop channels above the last selected output (smaller files, faster reads)',
            ),
            onChanged: state.busy || state.target >= 0
                ? null
                : (v) => state.setOptions(trim: v),
          ),
          CheckboxListTile(
            dense: true,
            controlAffinity: ListTileControlAffinity.leading,
            value: state.copyAudio,
            title: const Text('Copy matching audio (.mp3 / .wav)'),
            onChanged: state.busy || state.target >= 0
                ? null
                : (v) => state.setOptions(copyAudio: v),
          ),
          const SizedBox(height: 8),
          LinearProgressIndicator(value: state.busy ? state.progress : 0),
          const SizedBox(height: 8),
          Wrap(
            alignment: WrapAlignment.end,
            spacing: 8,
            runSpacing: 8,
            children: [
              OutlinedButton(
                onPressed: state.busy ? state.cancel : null,
                child: const Text('Cancel'),
              ),
              FilledButton(
                onPressed: state.busy
                    ? null
                    : () => run(context, state, state.convertSelected),
                child: const Text('Convert selected to card'),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

// ── Playlists ────────────────────────────────────────────────────────────────

class PlaylistsTab extends StatefulWidget {
  const PlaylistsTab({super.key, required this.state});
  final AppState state;

  @override
  State<PlaylistsTab> createState() => _PlaylistsTabState();
}

class _PlaylistsTabState extends State<PlaylistsTab>
    with AutomaticKeepAliveClientMixin {
  final _name = TextEditingController(text: 'default');
  final List<String> _entries = [];
  bool _repeat = true, _shuffle = false;

  @override
  bool get wantKeepAlive => true;

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  void _load(String file) {
    final doc = widget.state.playlists[file] ?? {};
    setState(() {
      _name.text = file.replaceFirst(
        RegExp(r'\.json$', caseSensitive: false),
        '',
      );
      _repeat = doc['repeat'] == true;
      _shuffle = doc['shuffle'] == true;
      _entries
        ..clear()
        ..addAll([for (final e in (doc['entries'] as List? ?? [])) '$e']);
    });
  }

  Widget _pane(String title, Widget body, {Widget? footer}) => Expanded(
    child: Card.outlined(
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Padding(
              padding: const EdgeInsets.all(4),
              child: Text(title, style: Theme.of(context).textTheme.titleSmall),
            ),
            Expanded(child: body),
            ?footer,
          ],
        ),
      ),
    ),
  );

  @override
  Widget build(BuildContext context) {
    super.build(context);
    final state = widget.state;
    final panes = [
      _pane(
        'Playlists on card',
        ListView(
          children: [
            for (final file in state.playlists.keys)
              ListTile(
                dense: true,
                title: Text(file),
                onTap: () => _load(file),
                trailing: IconButton(
                  tooltip: 'Delete $file',
                  icon: const Icon(Icons.delete_outline),
                  onPressed: () => run(context, state, () async {
                    final yes = await showDialog<bool>(
                      context: context,
                      builder: (c) => AlertDialog(
                        title: Text('Delete $file from the card folder?'),
                        actions: [
                          TextButton(
                            onPressed: () => Navigator.pop(c, false),
                            child: const Text('Cancel'),
                          ),
                          FilledButton(
                            onPressed: () => Navigator.pop(c, true),
                            child: const Text('Delete'),
                          ),
                        ],
                      ),
                    );
                    if (yes == true) await state.deletePlaylist(file);
                    return null;
                  }),
                ),
              ),
          ],
        ),
      ),
      _pane(
        'Sequences on card (tap to add)',
        ListView(
          children: [
            for (final s in state.cardSequences)
              ListTile(
                dense: true,
                title: Text(s),
                trailing: const Icon(Icons.add),
                onTap: () => setState(() => _entries.add(s)),
              ),
          ],
        ),
        footer: TextButton(
          onPressed: state.cardSequences.isEmpty
              ? null
              : () => setState(() => _entries.addAll(state.cardSequences)),
          child: const Text('Add all'),
        ),
      ),
      _pane(
        'Playlist',
        Column(
          children: [
            TextField(
              controller: _name,
              decoration: const InputDecoration(
                labelText: 'Name',
                isDense: true,
              ),
            ),
            Expanded(
              child: ReorderableListView(
                buildDefaultDragHandles: false,
                onReorderItem: (from, to) => setState(
                  () => _entries.insert(to, _entries.removeAt(from)),
                ),
                children: [
                  for (final (i, e) in _entries.indexed)
                    ListTile(
                      key: ValueKey('$i:$e'),
                      dense: true,
                      leading: ReorderableDragStartListener(
                        index: i,
                        child: const Icon(Icons.drag_handle),
                      ),
                      title: Text(e),
                      trailing: IconButton(
                        tooltip: 'Remove',
                        icon: const Icon(Icons.close),
                        onPressed: () => setState(() => _entries.removeAt(i)),
                      ),
                    ),
                ],
              ),
            ),
          ],
        ),
        footer: Wrap(
          crossAxisAlignment: WrapCrossAlignment.center,
          spacing: 4,
          children: [
            Checkbox(
              value: _repeat,
              onChanged: (v) => setState(() => _repeat = v ?? false),
            ),
            const Text('Repeat'),
            Checkbox(
              value: _shuffle,
              onChanged: (v) => setState(() => _shuffle = v ?? false),
            ),
            const Text('Shuffle'),
            const SizedBox(width: 8),
            FilledButton(
              onPressed: () => run(
                context,
                state,
                () => state.savePlaylist(
                  _name.text,
                  List.of(_entries),
                  _repeat,
                  _shuffle,
                ),
              ),
              child: const Text('Save playlist'),
            ),
          ],
        ),
      ),
    ];
    return Padding(
      padding: const EdgeInsets.all(12),
      child: LayoutBuilder(
        builder: (context, box) => box.maxWidth >= 760
            ? Row(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: panes,
              )
            : Column(children: panes),
      ),
    );
  }
}

// ── Schedule ─────────────────────────────────────────────────────────────────

class ScheduleTab extends StatefulWidget {
  const ScheduleTab({super.key, required this.state});
  final AppState state;

  @override
  State<ScheduleTab> createState() => _ScheduleTabState();
}

class _ScheduleTabState extends State<ScheduleTab>
    with AutomaticKeepAliveClientMixin {
  int _days = 0x7F;
  final _start = TextEditingController(text: '18:00');
  final _end = TextEditingController(text: '23:00');
  String? _playlist;

  @override
  bool get wantKeepAlive => true;

  @override
  void dispose() {
    _start.dispose();
    _end.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    super.build(context);
    final state = widget.state;
    final names = state.playlists.keys.toList();
    if (!names.contains(_playlist)) {
      _playlist = names.isEmpty ? null : names.first;
    }
    final boot = names.contains(state.bootPlaylist) ? state.bootPlaylist : '';
    Widget time(String label, TextEditingController c) => SizedBox(
      width: 90,
      child: TextField(
        controller: c,
        decoration: InputDecoration(
          labelText: label,
          hintText: 'HH:MM',
          isDense: true,
        ),
      ),
    );
    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (state.schedule.isEmpty)
            const Padding(
              padding: EdgeInsets.all(16),
              child: Center(child: Text('No schedule entries.')),
            )
          else
            for (final (i, e) in state.schedule.indexed)
              ListTile(
                dense: true,
                leading: const Icon(Icons.schedule),
                title: Text(
                  '${hhmm(e['start_hour'], e['start_min'])} - '
                  '${hhmm(e['end_hour'], e['end_min'])}   ${e['playlist'] ?? ''}',
                ),
                subtitle: Text(daysText((e['days'] as num?)?.toInt() ?? 0x7F)),
                trailing: IconButton(
                  tooltip: 'Remove entry',
                  icon: const Icon(Icons.delete_outline),
                  onPressed: () => state.removeSchedule(i),
                ),
              ),
          const Divider(),
          Wrap(
            spacing: 6,
            runSpacing: 6,
            crossAxisAlignment: WrapCrossAlignment.end,
            children: [
              for (var i = 0; i < 7; i++)
                FilterChip(
                  label: Text(dayNames[i]),
                  selected: (_days >> i) & 1 == 1,
                  onSelected: (v) => setState(
                    () => _days = v ? _days | (1 << i) : _days & ~(1 << i),
                  ),
                ),
              time('Start', _start),
              time('End', _end),
              DropdownMenu<String>(
                key: ValueKey('entry:${names.join('|')}'),
                label: const Text('Playlist'),
                width: 220,
                initialSelection: _playlist,
                dropdownMenuEntries: [
                  for (final n in names) DropdownMenuEntry(value: n, label: n),
                ],
                onSelected: (v) => setState(() => _playlist = v),
              ),
              OutlinedButton(
                onPressed: () => report(
                  context,
                  state.addSchedule(
                    _days,
                    _start.text,
                    _end.text,
                    _playlist ?? '',
                  ),
                ),
                child: const Text('Add entry'),
              ),
            ],
          ),
          const SizedBox(height: 4),
          Text(
            'Times are 24-hour. A window that runs past midnight is split into two entries, because the '
            'player cannot cross midnight in one.',
            style: Theme.of(context).textTheme.bodySmall,
          ),
          const SizedBox(height: 12),
          Wrap(
            spacing: 12,
            runSpacing: 8,
            crossAxisAlignment: WrapCrossAlignment.center,
            children: [
              DropdownMenu<String>(
                key: ValueKey('boot:$boot:${names.join('|')}'),
                label: const Text('Play at boot'),
                width: 220,
                initialSelection: boot,
                dropdownMenuEntries: [
                  const DropdownMenuEntry(value: '', label: '(none)'),
                  for (final n in names) DropdownMenuEntry(value: n, label: n),
                ],
                onSelected: (v) => state.setBootPlaylist(v ?? ''),
              ),
              ConstrainedBox(
                constraints: const BoxConstraints(maxWidth: 420),
                child: Text(
                  'Starts at power-on with no clock needed, and plays whenever no entry above is active.',
                  style: Theme.of(context).textTheme.bodySmall,
                ),
              ),
              FilledButton(
                onPressed: () => run(context, state, state.saveSchedule),
                child: const Text('Write schedule.json to card'),
              ),
            ],
          ),
        ],
      ),
    );
  }
}
