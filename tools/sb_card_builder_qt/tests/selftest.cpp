// Drives the real window with no person present: picks a show and a card folder,
// writes outputs, converts, saves a playlist and a schedule, and saves a screenshot
// of every tab.
//
//   sb_card_selftest <xLights show folder> <empty card folder> [screenshot folder]
//
// Exit code 0 means every step did what it should. Compare the card folder with one
// written by tools/sb_card_builder.py to check the output itself.
#include "mainwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QLineEdit>
#include <QRegularExpression>
#include <QListWidget>
#include <QTabWidget>
#include <QTextStream>
#include <QTimeEdit>
#include <QTimer>

static int failures = 0;
static void check(bool ok, const char* what) {
    QTextStream(stdout) << (ok ? "ok    " : "FAIL  ") << what << Qt::endl;
    if (!ok) failures++;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 3) {
        QTextStream(stdout) << "usage: sb_card_selftest <show folder> <card folder> [screenshot folder]" << Qt::endl;
        return 2;
    }
    const QString show = argv[1], card = argv[2], shots = argc > 3 ? argv[3] : "";
    MainWindow w;
    w.quiet = true;
    w.show();

    auto* tabs = w.findChild<QTabWidget*>();
    auto* sequences = w.findChild<QListWidget*>("sequences");
    auto* available = w.findChild<QListWidget*>("available");
    auto* entries = w.findChild<QListWidget*>("entries");
    auto* playlistName = w.findChild<QLineEdit*>("playlistName");
    auto* schedulePlaylist = w.findChild<QComboBox*>("schedulePlaylist");
    auto* bootPlaylist = w.findChild<QComboBox*>("bootPlaylist");
    auto* start = w.findChild<QTimeEdit*>("start");
    auto* end = w.findChild<QTimeEdit*>("end");
    QCheckBox* monday = nullptr;
    for (QCheckBox* c : w.findChildren<QCheckBox*>())
        if (c->text() == "Mon") monday = c;
    check(tabs && sequences && available && entries && playlistName && schedulePlaylist && bootPlaylist && start &&
              end && monday,
          "window has the expected widgets");
    if (failures) return 1;

    check(!w.writeOutputs() && w.notices.size() == 1, "refuses to write before a card folder is chosen");
    w.openShow(show);
    w.openCard(card);
    check(sequences->count() > 0, "lists the show's sequences");
    check(w.writeOutputs(), "writes outputs.json");

    QEventLoop loop;
    QObject::connect(&w, &MainWindow::conversionFinished, &loop, &QEventLoop::quit);
    QTimer::singleShot(10 * 60 * 1000, &loop, &QEventLoop::quit);
    check(w.convertSelected(), "starts converting");
    loop.exec();
    check(available->count() == sequences->count(), "every sequence reached the card");

    available->selectAll();
    for (const QListWidgetItem* item : available->selectedItems()) entries->addItem(item->text());
    playlistName->setText("xmas");
    check(w.savePlaylist(), "saves a playlist");

    monday->setChecked(false);
    start->setTime(QTime(17, 30));
    end->setTime(QTime(0, 30));
    schedulePlaylist->setCurrentText("xmas.json");
    check(w.addScheduleEntry(), "adds a schedule entry that crosses midnight");
    bootPlaylist->setCurrentText("xmas.json");
    check(bootPlaylist->currentText() == "xmas.json", "offers the playlist for play at boot");
    check(w.saveSchedule(), "writes schedule.json");
    check(QDir(card).entryList({"*.part", "*.tmp"}, QDir::Files).isEmpty() &&
              QDir(card + "/sequences").entryList(QDir::Files).filter(QRegularExpression("\\.[A-Za-z]{6}$")).isEmpty(),
          "leaves no temporary files behind");

    if (!shots.isEmpty()) {
        QDir().mkpath(shots);
        for (int i = 0; i < tabs->count(); i++) {
            tabs->setCurrentIndex(i);
            app.processEvents();
            w.grab().save(QDir(shots).filePath(QStringLiteral("qt_%1.png").arg(tabs->tabText(i))));
        }
    }
    // SB Pixel 16 export of the first controller, into its own folder
    auto* target = w.findChild<QComboBox*>("target");
    check(target && target->count() > 1, "offers an SB Pixel 16 export per controller");
    if (target && target->count() > 1) {
        w.openCard(card + "/pixel16");
        target->setCurrentIndex(1);
        check(w.convertSelected(), "starts the SB Pixel 16 export");
        loop.exec();
        const QDir p16(card + "/pixel16");
        check(p16.entryList(QDir::AllEntries | QDir::NoDotAndDotDot) == QStringList{"sequences"} &&
                  QDir(p16.filePath("sequences")).entryList({"*.fseq"}, QDir::Files).size() == sequences->count() &&
                  QDir(p16.filePath("sequences")).entryList(QDir::Files).size() == sequences->count(),
              "writes only sequences, with no audio, for the SB Pixel 16");
    }
    QTextStream(stdout) << (failures ? "FAILED" : "PASSED") << Qt::endl;
    return failures ? 1 : 0;
}
