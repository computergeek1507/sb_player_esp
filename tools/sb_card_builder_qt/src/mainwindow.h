#pragma once

#include "core/card.h"
#include "core/networks.h"

#include <QMainWindow>

#include <atomic>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QThread;
class QTimeEdit;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Each action returns false (after telling the user why) when it did nothing.
    void openShow(const QString& dir);
    void openCard(const QString& dir);
    bool writeOutputs();
    bool convertSelected();  // runs in the background; conversionFinished() follows
    bool savePlaylist();
    bool addScheduleEntry();
    bool saveSchedule();

    // When set, messages are collected in notices instead of shown in a dialog (used by the self test).
    bool quiet = false;
    QStringList notices;

signals:
    void conversionFinished();

private:
    QWidget* buildOutputsTab();
    QWidget* buildSequencesTab();
    QWidget* buildPlaylistsTab();
    QWidget* buildScheduleTab();

    void log(const QString& line);
    void notify(const QString& message);
    bool needCard();
    OutputsResult selectedOutputs() const;
    void refreshOutputs();
    void rescan();
    void reloadCard();
    void refreshSchedule();
    void setBusy(bool busy);

    QString m_show, m_card;
    QVector<Controller> m_controllers;
    QMap<QString, Json> m_playlists;
    Json m_schedule = Json::array();
    QThread* m_worker = nullptr;
    std::atomic<bool> m_cancel{false};
    bool m_refreshing = false;

    QTabWidget* m_tabs = nullptr;
    QLineEdit *m_showEdit = nullptr, *m_cardEdit = nullptr, *m_playlistName = nullptr;
    QPlainTextEdit *m_log = nullptr, *m_warnings = nullptr;
    QTableWidget *m_outputs = nullptr, *m_scheduleTable = nullptr;
    QListWidget *m_sequences = nullptr, *m_playlistList = nullptr, *m_available = nullptr, *m_entries = nullptr;
    QCheckBox *m_trim = nullptr, *m_audio = nullptr, *m_repeat = nullptr, *m_shuffle = nullptr;
    QCheckBox* m_days[7] = {};
    QProgressBar* m_progress = nullptr;
    QPushButton *m_convert = nullptr, *m_cancelButton = nullptr;
    QTimeEdit *m_start = nullptr, *m_end = nullptr;
    QComboBox *m_schedulePlaylist = nullptr, *m_bootPlaylist = nullptr, *m_target = nullptr;
};
