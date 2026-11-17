#include "mainwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimeEdit>

static const QString kNoBoot = QStringLiteral("(none)");
static const QString kPlayer = QStringLiteral("SB Player");

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QLatin1String(kAppName));
    resize(960, 700);

    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);
    auto* folders = new QGridLayout;
    auto addFolder = [&](int row, const QString& label, QLineEdit*& edit, void (MainWindow::*open)(const QString&)) {
        edit = new QLineEdit;
        edit->setReadOnly(true);
        edit->setPlaceholderText(QStringLiteral("not chosen"));
        auto* browse = new QPushButton(QStringLiteral("Browse..."));
        folders->addWidget(new QLabel(label), row, 0);
        folders->addWidget(edit, row, 1);
        folders->addWidget(browse, row, 2);
        connect(browse, &QPushButton::clicked, this, [this, label, open] {
            const QString dir = QFileDialog::getExistingDirectory(this, label);
            if (!dir.isEmpty()) (this->*open)(dir);
        });
    };
    addFolder(0, QStringLiteral("xLights show folder"), m_showEdit, &MainWindow::openShow);
    addFolder(1, QStringLiteral("SD card folder"), m_cardEdit, &MainWindow::openCard);
    root->addLayout(folders);

    m_tabs = new QTabWidget;
    m_tabs->addTab(buildOutputsTab(), QStringLiteral("Outputs"));
    m_tabs->addTab(buildSequencesTab(), QStringLiteral("Sequences"));
    m_tabs->addTab(buildPlaylistsTab(), QStringLiteral("Playlists"));
    m_tabs->addTab(buildScheduleTab(), QStringLiteral("Schedule"));
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(m_tabs);
    split->addWidget(m_log);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    root->addWidget(split, 1);
    setCentralWidget(central);
}

MainWindow::~MainWindow() {
    m_cancel = true;
    if (m_worker) m_worker->wait();
}

// ── helpers ──────────────────────────────────────────────────────────────────

void MainWindow::log(const QString& line) { m_log->appendPlainText(line); }

void MainWindow::notify(const QString& message) {
    if (quiet)
        notices << message;
    else
        QMessageBox::information(this, QLatin1String(kAppName), message);
}

bool MainWindow::needCard() {
    if (!m_card.isEmpty()) return true;
    notify(QStringLiteral("Choose the SD card folder first."));
    return false;
}

void MainWindow::openShow(const QString& dir) {
    m_show = dir;
    m_showEdit->setText(QDir::toNativeSeparators(dir));
    m_controllers.clear();
    const QString net = QDir(dir).filePath(QStringLiteral("xlights_networks.xml"));
    try {
        m_controllers = parseNetworks(net);
    } catch (const std::exception& e) {
        log(QStringLiteral("Could not read %1: %2").arg(QDir::toNativeSeparators(net), QString::fromUtf8(e.what())));
    }
    m_refreshing = true;
    m_outputs->setRowCount(m_controllers.size());
    for (int i = 0; i < m_controllers.size(); i++) {
        const Controller& c = m_controllers[i];
        const QStringList cells = {c.name, c.protocol, c.ip, c.state, QString::number(c.start + 1),
                                   QString::number(c.channels), c.universeText()};
        for (int col = 0; col < cells.size(); col++) {
            auto* item = new QTableWidgetItem(cells[col]);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | (col == 0 ? Qt::ItemIsUserCheckable : Qt::NoItemFlags));
            if (col == 0)
                item->setCheckState(c.usable() && c.state == QLatin1String("Active") ? Qt::Checked : Qt::Unchecked);
            m_outputs->setItem(i, col, item);
        }
    }
    m_outputs->resizeColumnsToContents();
    m_refreshing = false;
    // Export targets: the player, or any one controller as an SB Pixel 16 (item data = controller index)
    m_target->clear();
    m_target->addItem(kPlayer, -1);
    for (int i = 0; i < m_controllers.size(); i++)
        if (m_controllers[i].channels) m_target->addItem(QStringLiteral("SB Pixel 16: ") + m_controllers[i].name, i);
    refreshOutputs();
    rescan();
}

void MainWindow::openCard(const QString& dir) {
    m_card = dir;
    m_cardEdit->setText(QDir::toNativeSeparators(dir));
    reloadCard();
    const Json sched = readJson(QDir(dir).filePath(QStringLiteral("schedule.json")));
    m_schedule = jsonArray(sched, "entries");
    const QString boot = jsonString(sched, "boot_playlist");
    m_bootPlaylist->setCurrentText(m_playlists.contains(boot) ? boot : kNoBoot);
    refreshSchedule();
    log(QStringLiteral("Card folder: %1 sequences, %2 playlists, %3 schedule entries")
            .arg(m_available->count()).arg(m_playlists.size()).arg(m_schedule.size()));
}

void MainWindow::reloadCard() {
    if (m_card.isEmpty()) return;
    m_available->clear();
    m_available->addItems(listCardSequences(m_card));
    m_playlists = loadPlaylists(m_card);
    m_playlistList->clear();
    m_playlistList->addItems(m_playlists.keys());
    const QString boot = m_bootPlaylist->currentText(), entry = m_schedulePlaylist->currentText();
    m_schedulePlaylist->clear();
    m_schedulePlaylist->addItems(m_playlists.keys());
    if (m_playlists.contains(entry)) m_schedulePlaylist->setCurrentText(entry);
    m_bootPlaylist->clear();
    m_bootPlaylist->addItem(kNoBoot);
    m_bootPlaylist->addItems(m_playlists.keys());
    m_bootPlaylist->setCurrentText(m_playlists.contains(boot) ? boot : kNoBoot);
}

void MainWindow::setBusy(bool busy) {
    m_convert->setEnabled(!busy);
    m_cancelButton->setEnabled(busy);
    if (!busy) m_progress->setValue(0);
}

// ── Outputs ──────────────────────────────────────────────────────────────────

QWidget* MainWindow::buildOutputsTab() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    m_outputs = new QTableWidget(0, 7);
    m_outputs->setHorizontalHeaderLabels({QStringLiteral("Controller"), QStringLiteral("Protocol"),
                                          QStringLiteral("Address"), QStringLiteral("xLights state"),
                                          QStringLiteral("Start ch"), QStringLiteral("Channels"),
                                          QStringLiteral("Universes")});
    m_outputs->verticalHeader()->hide();
    m_outputs->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_outputs->horizontalHeader()->setStretchLastSection(true);
    connect(m_outputs, &QTableWidget::itemChanged, this, [this] {
        if (!m_refreshing) refreshOutputs();
    });
    v->addWidget(m_outputs, 1);
    v->addWidget(new QLabel(QStringLiteral(
        "Tick the controllers the player should drive. Only controllers marked 'Active' in xLights are ticked by default.")));
    m_warnings = new QPlainTextEdit;
    m_warnings->setReadOnly(true);
    m_warnings->setMaximumHeight(90);
    v->addWidget(m_warnings);
    auto* write = new QPushButton(QStringLiteral("Write outputs.json to card"));
    connect(write, &QPushButton::clicked, this, &MainWindow::writeOutputs);
    v->addWidget(write, 0, Qt::AlignRight);
    return page;
}

OutputsResult MainWindow::selectedOutputs() const {
    QVector<Controller> use;
    for (int i = 0; i < m_controllers.size(); i++)
        if (m_outputs->item(i, 0) && m_outputs->item(i, 0)->checkState() == Qt::Checked) use.append(m_controllers[i]);
    return buildOutputs(use);
}

void MainWindow::refreshOutputs() {
    const OutputsResult o = selectedOutputs();
    m_warnings->setPlainText(!o.warnings.isEmpty() ? o.warnings.join('\n')
                             : m_controllers.isEmpty() ? QString()
                             : QStringLiteral("%1 E1.31 and %2 DDP outputs, %3 channels.")
                                   .arg(o.e131Count).arg(o.ddpCount).arg(o.channelsNeeded));
}

bool MainWindow::writeOutputs() {
    if (!needCard()) return false;
    const OutputsResult o = selectedOutputs();
    if (!o.e131Count && !o.ddpCount) {
        notify(QStringLiteral("No usable controllers are selected."));
        return false;
    }
    try {
        writeJson(QDir(m_card).filePath(QStringLiteral("outputs.json")), o.doc);
    } catch (const std::exception& e) {
        notify(QString::fromUtf8(e.what()));
        return false;
    }
    log(QStringLiteral("Wrote outputs.json: %1 E1.31, %2 DDP outputs").arg(o.e131Count).arg(o.ddpCount));
    for (const QString& w : o.warnings) log(QStringLiteral("   WARNING ") + w);
    return true;
}

// ── Sequences ────────────────────────────────────────────────────────────────

QWidget* MainWindow::buildSequencesTab() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    m_sequences = new QListWidget;
    m_sequences->setObjectName(QStringLiteral("sequences"));
    v->addWidget(m_sequences, 1);
    auto* bar = new QHBoxLayout;
    auto* add = new QPushButton(QStringLiteral("Add files..."));
    auto* scan = new QPushButton(QStringLiteral("Rescan show folder"));
    auto* all = new QPushButton(QStringLiteral("Tick all"));
    auto* none = new QPushButton(QStringLiteral("Tick none"));
    for (QPushButton* b : {add, scan, all, none}) bar->addWidget(b);
    bar->addStretch(1);
    v->addLayout(bar);
    connect(add, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("FSEQ files"), m_show,
                                                                QStringLiteral("FSEQ sequences (*.fseq);;All files (*)"));
        for (const QString& f : files) {
            if (!m_sequences->findItems(QDir::toNativeSeparators(f), Qt::MatchExactly).isEmpty()) continue;
            auto* item = new QListWidgetItem(QDir::toNativeSeparators(f), m_sequences);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
        }
    });
    connect(scan, &QPushButton::clicked, this, &MainWindow::rescan);
    auto tick = [this](Qt::CheckState s) {
        for (int i = 0; i < m_sequences->count(); i++) m_sequences->item(i)->setCheckState(s);
    };
    connect(all, &QPushButton::clicked, this, [tick] { tick(Qt::Checked); });
    connect(none, &QPushButton::clicked, this, [tick] { tick(Qt::Unchecked); });

    auto* target = new QHBoxLayout;
    m_target = new QComboBox;
    m_target->setObjectName(QStringLiteral("target"));
    m_target->setMinimumWidth(260);
    m_target->addItem(kPlayer, -1);
    target->addWidget(new QLabel(QStringLiteral("Export for")));
    target->addWidget(m_target);
    target->addWidget(new QLabel(QStringLiteral("an SB Pixel 16 gets only its own channels, and no audio")), 1);
    v->addLayout(target);
    connect(m_target, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_trim->setEnabled(index <= 0);
        m_audio->setEnabled(index <= 0);
    });
    m_trim = new QCheckBox(QStringLiteral("Drop channels above the last selected output (smaller files, faster reads)"));
    m_audio = new QCheckBox(QStringLiteral("Copy matching audio (.mp3 / .wav)"));
    m_trim->setChecked(true);
    m_audio->setChecked(true);
    v->addWidget(m_trim);
    v->addWidget(m_audio);

    auto* run = new QHBoxLayout;
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"));
    m_cancelButton->setEnabled(false);
    m_convert = new QPushButton(QStringLiteral("Convert ticked to card"));
    run->addWidget(m_progress, 1);
    run->addWidget(m_convert);
    run->addWidget(m_cancelButton);
    v->addLayout(run);
    connect(m_convert, &QPushButton::clicked, this, &MainWindow::convertSelected);
    connect(m_cancelButton, &QPushButton::clicked, this, [this] { m_cancel = true; });
    return page;
}

void MainWindow::rescan() {
    if (m_show.isEmpty()) return;
    m_sequences->clear();
    const QDir dir(m_show);
    for (const QString& n : dir.entryList({QStringLiteral("*.fseq")}, QDir::Files, QDir::Name)) {
        auto* item = new QListWidgetItem(QDir::toNativeSeparators(dir.filePath(n)), m_sequences);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
}

bool MainWindow::convertSelected() {
    if (m_worker || !needCard()) return false;
    QStringList files;
    for (int i = 0; i < m_sequences->count(); i++)
        if (m_sequences->item(i)->checkState() == Qt::Checked) files << m_sequences->item(i)->text();
    if (files.isEmpty()) {
        notify(QStringLiteral("Tick one or more sequences."));
        return false;
    }
    Layout layout;
    bool audio = m_audio->isChecked();
    const int pixel16 = m_target->currentData().toInt();  // controller index, or -1 for the player
    if (pixel16 >= 0 && pixel16 < m_controllers.size()) {
        layout = pixel16Layout(m_controllers[pixel16]);
        audio = false;
    } else {
        layout.maxChannels = m_trim->isChecked() ? selectedOutputs().channelsNeeded : 0;
        if (m_trim->isChecked() && !layout.maxChannels)
            log(QStringLiteral("No outputs selected, so channels are not trimmed."));
    }
    const Controller controller = pixel16 >= 0 && pixel16 < m_controllers.size() ? m_controllers[pixel16] : Controller();
    m_cancel = false;
    setBusy(true);
    const QString card = m_card;
    const QStringList dirs = m_show.isEmpty() ? QStringList{} : QStringList{m_show};
    // Everything that touches widgets is posted back to the GUI thread.
    auto post = [this](std::function<void()> fn) { QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection); };
    m_worker = QThread::create([=] {
        int ok = 0, shown = -1;
        for (int n = 0; n < files.size(); n++) {
            try {
                exportSequence(
                    files[n], card, layout, audio, dirs, [=](const QString& line) { post([=] { log(line); }); },
                    [&, n](quint32 done, quint32 total) {
                        const int value = int((n + double(done) / total) * 1000 / files.size());
                        if (value == shown) return;
                        shown = value;
                        post([=] { m_progress->setValue(value); });
                    },
                    [this] { return m_cancel.load(); });
                ok++;
            } catch (const Cancelled&) {
                post([=] { log(QStringLiteral("Cancelled.")); });
                break;
            } catch (const std::exception& e) {
                const QString why = QString::fromUtf8(e.what());
                post([=] { log(QStringLiteral("%1: FAILED - %2").arg(QFileInfo(files[n]).fileName(), why)); });
            }
        }
        if (pixel16 >= 0 && ok)
            for (const QString& note : pixel16Notes(controller, card)) post([=] { log(note); });
        post([=] { log(QStringLiteral("Converted %1 of %2 sequences.").arg(ok).arg(files.size())); });
    });
    connect(m_worker, &QThread::finished, this, [this] {
        m_worker->deleteLater();
        m_worker = nullptr;
        setBusy(false);
        reloadCard();
        emit conversionFinished();
    });
    m_worker->start();
    return true;
}

// ── Playlists ────────────────────────────────────────────────────────────────

QWidget* MainWindow::buildPlaylistsTab() {
    auto* page = new QWidget;
    auto* h = new QHBoxLayout(page);
    auto column = [h](const QString& title, int stretch) {
        auto* v = new QVBoxLayout;
        v->addWidget(new QLabel(title));
        h->addLayout(v, stretch);
        return v;
    };

    auto* left = column(QStringLiteral("Playlists on card"), 2);
    m_playlistList = new QListWidget;
    left->addWidget(m_playlistList, 1);
    auto* del = new QPushButton(QStringLiteral("Delete"));
    left->addWidget(del);
    connect(m_playlistList, &QListWidget::currentTextChanged, this, [this](const QString& name) {
        if (!m_playlists.contains(name)) return;
        const Json doc = m_playlists.value(name);
        m_playlistName->setText(name.chopped(name.endsWith(QLatin1String(".json"), Qt::CaseInsensitive) ? 5 : 0));
        m_repeat->setChecked(jsonBool(doc, "repeat"));
        m_shuffle->setChecked(jsonBool(doc, "shuffle"));
        m_entries->clear();
        for (const Json& e : jsonArray(doc, "entries"))
            if (e.is_string()) m_entries->addItem(QString::fromStdString(e.get<std::string>()));
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const QListWidgetItem* item = m_playlistList->currentItem();
        if (!item) return;
        const QString name = item->text();
        if (!quiet && QMessageBox::question(this, QLatin1String(kAppName),
                                            QStringLiteral("Delete playlist %1 from the card folder?").arg(name)) !=
                          QMessageBox::Yes)
            return;
        QFile::remove(QDir(m_card).filePath(QStringLiteral("playlists/") + name));
        log(QStringLiteral("Deleted playlists/%1").arg(name));
        reloadCard();
    });

    auto* mid = column(QStringLiteral("Sequences on card"), 3);
    m_available = new QListWidget;
    m_available->setObjectName(QStringLiteral("available"));
    m_available->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mid->addWidget(m_available, 1);
    auto* add = new QPushButton(QStringLiteral("Add to playlist >>"));
    mid->addWidget(add);
    auto addSelected = [this] {
        for (const QListWidgetItem* item : m_available->selectedItems()) m_entries->addItem(item->text());
    };
    connect(add, &QPushButton::clicked, this, addSelected);
    connect(m_available, &QListWidget::itemDoubleClicked, this, addSelected);

    auto* right = column(QStringLiteral("Playlist (drag to reorder)"), 3);
    m_playlistName = new QLineEdit(QStringLiteral("default"));
    m_playlistName->setObjectName(QStringLiteral("playlistName"));
    m_playlistName->setPlaceholderText(QStringLiteral("Name"));
    right->addWidget(m_playlistName);
    m_entries = new QListWidget;
    m_entries->setObjectName(QStringLiteral("entries"));
    m_entries->setDragDropMode(QAbstractItemView::InternalMove);
    m_entries->setSelectionMode(QAbstractItemView::ExtendedSelection);
    right->addWidget(m_entries, 1);
    auto* bar = new QHBoxLayout;
    auto* remove = new QPushButton(QStringLiteral("Remove"));
    m_repeat = new QCheckBox(QStringLiteral("Repeat"));
    m_repeat->setChecked(true);
    m_shuffle = new QCheckBox(QStringLiteral("Shuffle"));
    auto* save = new QPushButton(QStringLiteral("Save playlist"));
    bar->addWidget(remove);
    bar->addWidget(m_repeat);
    bar->addWidget(m_shuffle);
    bar->addStretch(1);
    bar->addWidget(save);
    right->addLayout(bar);
    connect(remove, &QPushButton::clicked, this, [this] { qDeleteAll(m_entries->selectedItems()); });
    connect(save, &QPushButton::clicked, this, &MainWindow::savePlaylist);
    return page;
}

bool MainWindow::savePlaylist() {
    if (!needCard()) return false;
    const QString name = safePlaylistName(m_playlistName->text());
    Json entries = Json::array();
    for (int i = 0; i < m_entries->count(); i++) entries.push_back(m_entries->item(i)->text().toStdString());
    if (name.isEmpty() || name.size() > 50 || entries.empty()) {
        notify(QStringLiteral("A playlist needs a name (up to 50 characters) and at least one sequence."));
        return false;
    }
    if (int(entries.size()) > kMaxPlaylistEntries) {
        notify(QStringLiteral("The player reads at most %1 entries per playlist.").arg(kMaxPlaylistEntries));
        return false;
    }
    try {
        Json doc = Json::object();
        doc["repeat"] = m_repeat->isChecked();
        doc["shuffle"] = m_shuffle->isChecked();
        doc["entries"] = entries;
        writeJson(QDir(m_card).filePath(QStringLiteral("playlists/%1.json").arg(name)), doc);
    } catch (const std::exception& e) {
        notify(QString::fromUtf8(e.what()));
        return false;
    }
    log(QStringLiteral("Wrote playlists/%1.json (%2 sequences)").arg(name).arg(entries.size()));
    reloadCard();
    return true;
}

// ── Schedule ─────────────────────────────────────────────────────────────────

QWidget* MainWindow::buildScheduleTab() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    m_scheduleTable = new QTableWidget(0, 4);
    m_scheduleTable->setHorizontalHeaderLabels(
        {QStringLiteral("Days"), QStringLiteral("Start"), QStringLiteral("End"), QStringLiteral("Playlist")});
    m_scheduleTable->verticalHeader()->hide();
    m_scheduleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_scheduleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_scheduleTable->horizontalHeader()->setStretchLastSection(true);
    m_scheduleTable->setColumnWidth(0, 260);
    v->addWidget(m_scheduleTable, 1);

    auto* form = new QHBoxLayout;
    for (int i = 0; i < 7; i++) {
        m_days[i] = new QCheckBox(QLatin1String(kDayNames[i]));
        m_days[i]->setChecked(true);
        form->addWidget(m_days[i]);
    }
    m_start = new QTimeEdit(QTime(18, 0));
    m_start->setObjectName(QStringLiteral("start"));
    m_end = new QTimeEdit(QTime(23, 0));
    m_end->setObjectName(QStringLiteral("end"));
    for (QTimeEdit* t : {m_start, m_end}) t->setDisplayFormat(QStringLiteral("HH:mm"));
    m_schedulePlaylist = new QComboBox;
    m_schedulePlaylist->setObjectName(QStringLiteral("schedulePlaylist"));
    m_schedulePlaylist->setMinimumWidth(160);
    form->addSpacing(12);
    form->addWidget(new QLabel(QStringLiteral("Start")));
    form->addWidget(m_start);
    form->addWidget(new QLabel(QStringLiteral("End")));
    form->addWidget(m_end);
    form->addWidget(new QLabel(QStringLiteral("Playlist")));
    form->addWidget(m_schedulePlaylist);
    form->addStretch(1);
    v->addLayout(form);

    auto* bar = new QHBoxLayout;
    auto* add = new QPushButton(QStringLiteral("Add entry"));
    auto* remove = new QPushButton(QStringLiteral("Remove selected"));
    bar->addWidget(add);
    bar->addWidget(remove);
    bar->addStretch(1);
    v->addLayout(bar);
    v->addWidget(new QLabel(QStringLiteral("A window that runs past midnight is split into two entries, because the "
                                           "player cannot cross midnight in one.")));
    connect(add, &QPushButton::clicked, this, &MainWindow::addScheduleEntry);
    connect(remove, &QPushButton::clicked, this, [this] {
        QList<int> rows;
        for (const QModelIndex& i : m_scheduleTable->selectionModel()->selectedRows()) rows << i.row();
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        for (int row : rows) m_schedule.erase(m_schedule.begin() + row);
        refreshSchedule();
    });

    auto* boot = new QHBoxLayout;
    m_bootPlaylist = new QComboBox;
    m_bootPlaylist->setObjectName(QStringLiteral("bootPlaylist"));
    m_bootPlaylist->setMinimumWidth(160);
    m_bootPlaylist->addItem(kNoBoot);
    auto* save = new QPushButton(QStringLiteral("Write schedule.json to card"));
    boot->addWidget(new QLabel(QStringLiteral("Play at boot")));
    boot->addWidget(m_bootPlaylist);
    boot->addWidget(new QLabel(QStringLiteral("starts at power-on with no clock needed, and plays whenever no entry is active")), 1);
    boot->addWidget(save);
    v->addLayout(boot);
    connect(save, &QPushButton::clicked, this, &MainWindow::saveSchedule);
    return page;
}

void MainWindow::refreshSchedule() {
    const int rows = int(m_schedule.size());
    m_scheduleTable->setRowCount(rows);
    auto hhmm = [](const Json& e, const char* h, const char* m) {
        return QStringLiteral("%1:%2").arg(jsonInt(e, h), 2, 10, QLatin1Char('0')).arg(jsonInt(e, m), 2, 10, QLatin1Char('0'));
    };
    for (int i = 0; i < rows; i++) {
        const Json& e = m_schedule[size_t(i)];
        const QStringList cells = {daysText(jsonInt(e, "days", 0x7F)), hhmm(e, "start_hour", "start_min"),
                                   hhmm(e, "end_hour", "end_min"), jsonString(e, "playlist")};
        for (int col = 0; col < cells.size(); col++) m_scheduleTable->setItem(i, col, new QTableWidgetItem(cells[col]));
    }
}

bool MainWindow::addScheduleEntry() {
    int days = 0;
    for (int i = 0; i < 7; i++)
        if (m_days[i]->isChecked()) days |= 1 << i;
    const QString playlist = m_schedulePlaylist->currentText();
    if (!days || playlist.isEmpty()) {
        notify(QStringLiteral("Pick at least one day and a playlist (save a playlist first)."));
        return false;
    }
    const Json entries = scheduleEntries(days, m_start->time().msecsSinceStartOfDay() / 60000,
                                               m_end->time().msecsSinceStartOfDay() / 60000, playlist);
    if (int(m_schedule.size() + entries.size()) > kMaxScheduleEntries) {
        notify(QStringLiteral("The player reads at most %1 schedule entries.").arg(kMaxScheduleEntries));
        return false;
    }
    for (const Json& e : entries) m_schedule.push_back(e);
    refreshSchedule();
    return true;
}

bool MainWindow::saveSchedule() {
    if (!needCard()) return false;
    const QString boot = m_bootPlaylist->currentText() == kNoBoot ? QString() : m_bootPlaylist->currentText();
    if (!boot.isEmpty() && !jsonBool(m_playlists.value(boot), "repeat"))
        log(QStringLiteral("   NOTE %1 does not have Repeat ticked, so it will play once and stop").arg(boot));
    try {
        Json doc = Json::object();
        doc["boot_playlist"] = boot.toStdString();
        doc["entries"] = m_schedule;
        writeJson(QDir(m_card).filePath(QStringLiteral("schedule.json")), doc);
    } catch (const std::exception& e) {
        notify(QString::fromUtf8(e.what()));
        return false;
    }
    log(QStringLiteral("Wrote schedule.json (%1 entries, play at boot: %2)")
            .arg(m_schedule.size()).arg(boot.isEmpty() ? QStringLiteral("none") : boot));
    return true;
}
