// Command line version of SB Card Builder. Same options as tools/sb_card_builder.py.
#include "core/card.h"
#include "core/networks.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QTextStream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sb_card_cli"));
    QTextStream out(stdout);

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral(
        "Prepare an SD card for the ESP32-P4 SB Player from an xLights show folder.\n"
        "Writes outputs.json and converts sequences to the uncompressed FSEQ v2 the player reads."));
    p.addHelpOption();
    p.addOptions({
        {"show", "xLights show folder (reads xlights_networks.xml and *.fseq)", "dir"},
        {"out", "SD card folder to write", "dir"},
        {"fseq", "sequence to convert; repeat for more (default: every .fseq in --show)", "file"},
        {"all-controllers", "include Inactive / xLights Only controllers"},
        {"no-trim", "keep channels above the last output"},
        {"no-audio", "do not copy audio files"},
        {"pixel16", "export for an SB Pixel 16: keep only this xLights controller's channels "
                    "(no outputs.json, no audio)", "controller"},
        {"firmware", "copy a player firmware image (SBPlayer.ino.bin) to the card; installed at next boot", "bin"},
        {"littlefs", "copy a player web pages image (littlefs.bin) to the card; installed at next boot", "bin"},
        {"info", "print an FSEQ file's header and exit", "file"},
    });
    p.process(app);

    try {
        if (p.isSet("info")) {
            QFile f(p.value("info"));
            if (!f.open(QIODevice::ReadOnly)) throw FseqError(QStringLiteral("cannot open %1").arg(f.fileName()));
            out << readFseqInfo(f).describe() << Qt::endl;
            return 0;
        }
        if (!p.isSet("out")) p.showHelp(2);
        const QString show = p.value("show"), outDir = p.value("out");
        int updateFailed = 0;
        for (const auto& [option, kind] : {std::pair{"littlefs", UpdateKind::LittleFs},
                                           std::pair{"firmware", UpdateKind::Firmware}}) {
            if (!p.isSet(QLatin1String(option))) continue;
            try {
                out << copyUpdate(p.value(QLatin1String(option)), outDir, kind) << Qt::endl;
            } catch (const std::exception& e) {
                updateFailed++;
                out << "FAILED - " << e.what() << Qt::endl;
            }
        }
        Layout layout;
        Controller pixel16;
        const bool forPixel16 = p.isSet("pixel16");
        const QString net = QDir(show).filePath(QStringLiteral("xlights_networks.xml"));
        if (forPixel16) {
            if (!p.isSet("show")) throw FseqError(QStringLiteral("--pixel16 needs --show"));
            bool found = false;
            for (const Controller& c : parseNetworks(net))
                if (!found && c.channels && c.name.compare(p.value("pixel16"), Qt::CaseInsensitive) == 0) {
                    pixel16 = c;
                    found = true;
                }
            if (!found)
                throw FseqError(QStringLiteral("no controller named '%1' in xlights_networks.xml").arg(p.value("pixel16")));
            layout = pixel16Layout(pixel16);
        } else if (p.isSet("show") && QFile::exists(net)) {
            QVector<Controller> use;
            for (const Controller& c : parseNetworks(net))
                if (p.isSet("all-controllers") || c.state == QLatin1String("Active")) use.append(c);
            const OutputsResult o = buildOutputs(use);
            for (const QString& w : o.warnings) out << "WARNING " << w << Qt::endl;
            writeJson(QDir(outDir).filePath(QStringLiteral("outputs.json")), o.doc);
            out << QStringLiteral("outputs.json: %1 E1.31, %2 DDP outputs, %3 channels")
                       .arg(o.e131Count).arg(o.ddpCount).arg(o.channelsNeeded) << Qt::endl;
            layout.maxChannels = p.isSet("no-trim") ? 0 : o.channelsNeeded;
        }
        QStringList files = p.values("fseq");
        if (!p.isSet("fseq") && p.isSet("show"))
            for (const QString& n : QDir(show).entryList({QStringLiteral("*.fseq")}, QDir::Files, QDir::Name))
                files << QDir(show).filePath(n);
        int failed = 0;
        for (const QString& src : files) {
            try {
                exportSequence(src, outDir, layout, !p.isSet("no-audio") && !forPixel16,
                               p.isSet("show") ? QStringList{show} : QStringList{},
                               [&out](const QString& line) { out << line << Qt::endl; });
            } catch (const std::exception& e) {
                failed++;
                out << QFileInfo(src).fileName() << ": FAILED - " << e.what() << Qt::endl;
            }
        }
        if (forPixel16 && files.size() > failed)
            for (const QString& note : pixel16Notes(pixel16, outDir)) out << note << Qt::endl;
        return failed || updateFailed ? 1 : 0;
    } catch (const std::exception& e) {
        out << "ERROR " << e.what() << Qt::endl;
        return 1;
    }
}
