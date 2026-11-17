#include "core/networks.h"

#include <QFile>
#include <QRegularExpression>

#include <pugixml.hpp>

#include <cstring>
#include <stdexcept>

QString Controller::universeText() const {
    if (protocol != QLatin1String("E131") || universes.isEmpty()) return {};
    if (universes.size() == 1) return QString::number(universes.first().first);
    return QStringLiteral("%1-%2").arg(universes.first().first).arg(universes.last().first);
}

static bool has(const pugi::xml_node& n, const char* name) { return !n.attribute(name).empty(); }
static QString attr(const pugi::xml_node& n, const char* name) { return QString::fromUtf8(n.attribute(name).value()); }
static QString attr(const pugi::xml_node& n, const char* name, const char* fallback) {
    return has(n, name) ? attr(n, name) : QString::fromLatin1(fallback);
}
static int intAttr(const pugi::xml_node& n, const char* name, int fallback) {
    bool ok = false;
    const int v = attr(n, name).toInt(&ok);
    return ok ? v : fallback;
}

static void addNetwork(Controller& c, const pugi::xml_node& n) {
    const int size = intAttr(n, "MaxChannels", 512);
    int count = 1;
    if (attr(n, "NetworkType") == QLatin1String("E131")) count = qMax(1, intAttr(n, "NumUniverses", 1));
    const int first = intAttr(n, "BaudRate", 1);  // xLights stores the universe here
    for (int i = 0; i < count; i++) {
        c.universes.append({first + i, size});
        c.channels += size;
    }
    if (attr(n, "KeepChannelNumbers") == QLatin1String("0")) c.keepNumbers = false;
}

QVector<Controller> parseNetworks(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error(QStringLiteral("cannot open %1").arg(path).toStdString());
    const QByteArray data = f.readAll();
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(data.constData(), size_t(data.size()));
    if (!parsed)
        throw std::runtime_error(QStringLiteral("%1: %2").arg(path, QLatin1String(parsed.description())).toStdString());

    QVector<Controller> out;
    QString prevKey;
    for (const pugi::xml_node& node : doc.document_element().children()) {
        if (!std::strcmp(node.name(), "Controller")) {
            Controller c;
            c.name = attr(node, "Name", "?");
            c.protocol = attr(node, "Protocol");
            c.ip = attr(node, "IP");
            if (has(node, "ActiveState")) c.state = attr(node, "ActiveState");
            bool first = true;
            for (const pugi::xml_node& net : node.children("network")) {
                if (first) {
                    if (c.protocol.isEmpty()) c.protocol = attr(net, "NetworkType");
                    if (c.ip.isEmpty()) c.ip = attr(net, "ComPort");
                    first = false;
                }
                addNetwork(c, net);
            }
            if (c.protocol.isEmpty()) c.protocol = attr(node, "Type", "?");
            out.append(c);
            prevKey.clear();
        } else if (!std::strcmp(node.name(), "network")) {
            // pre-2020 files: bare outputs, group runs that go to the same device
            const QString enabled = attr(node, "Enabled", "Yes");
            const QString key = attr(node, "NetworkType") + '|' + attr(node, "ComPort") + '|' + enabled;
            if (key != prevKey) {
                Controller c;
                c.name = !attr(node, "Description").isEmpty() ? attr(node, "Description") : attr(node, "ComPort", "?");
                c.protocol = attr(node, "NetworkType", "?");
                c.ip = attr(node, "ComPort");
                c.state = enabled != QLatin1String("No") ? QStringLiteral("Active") : QStringLiteral("Inactive");
                out.append(c);
                prevKey = key;
            }
            addNetwork(out.last(), node);
        }
    }
    qint64 ch = 0;
    for (Controller& c : out) {  // every controller takes channel space, active or not
        c.start = ch;
        ch += c.channels;
    }
    return out;
}

OutputsResult buildOutputs(const QVector<Controller>& controllers) {
    static const QRegularExpression ipv4(QStringLiteral("^(\\d{1,3}\\.){3}\\d{1,3}$"));
    Json e131 = Json::array(), ddp = Json::array();
    OutputsResult r;
    for (const Controller& c : controllers) {
        if (!c.usable()) {
            r.warnings << QStringLiteral("%1: %2 is not supported by the player - skipped").arg(c.name, c.protocol);
            continue;
        }
        const bool multicast = c.ip.compare(QLatin1String("MULTICAST"), Qt::CaseInsensitive) == 0;
        if (!multicast && !ipv4.match(c.ip).hasMatch()) {
            r.warnings << QStringLiteral("%1: address '%2' is not an IPv4 address (host names are not supported) - skipped")
                              .arg(c.name, c.ip);
            continue;
        }
        if (c.protocol == QLatin1String("DDP")) {
            if (multicast) {
                r.warnings << QStringLiteral("%1: DDP cannot be multicast - skipped").arg(c.name);
                continue;
            }
            if (!c.keepNumbers && c.start != 0)
                r.warnings << QStringLiteral("%1: 'Keep Channel Numbers' is off in xLights, but the player always sends "
                                             "absolute channel offsets - this controller will show the wrong data")
                                  .arg(c.name);
            Json o = Json::object();
            o["enabled"] = true;
            o["ip"] = c.ip.toStdString();
            o["channel_start"] = c.start;
            o["channel_count"] = c.channels;
            ddp.push_back(std::move(o));
            continue;
        }
        // E1.31: one player output = consecutive universes of one size (the last may be shorter)
        struct Run { int universe, size; qint64 start, count; int next; bool isShort; };
        QVector<Run> runs;
        qint64 ch = c.start;
        for (const auto& u : c.universes) {
            if (!runs.isEmpty() && u.first == runs.last().next && !runs.last().isShort && u.second <= runs.last().size) {
                Run& run = runs.last();
                run.count += u.second;
                run.next++;
                run.isShort = u.second < run.size;
            } else {
                runs.append({u.first, u.second, ch, u.second, u.first + 1, false});
            }
            ch += u.second;
        }
        if (runs.size() > 1)
            r.warnings << QStringLiteral("%1: universes are not consecutive / equal sized - uses %2 player outputs")
                              .arg(c.name).arg(runs.size());
        for (const Run& run : runs) {
            Json o = Json::object();
            o["enabled"] = true;
            o["universe_start"] = run.universe;
            o["universe_size"] = run.size;
            o["channel_start"] = run.start;
            o["channel_count"] = run.count;
            o["multicast"] = multicast;
            o["unicast_ip"] = multicast ? std::string() : c.ip.toStdString();
            e131.push_back(std::move(o));
        }
    }
    auto cap = [&r](const char* kind, Json& list, int max) {
        const int have = int(list.size());
        if (have <= max) return;
        r.warnings << QStringLiteral("%1 %2 outputs needed but the player supports %3 - the last %4 were dropped")
                          .arg(have).arg(QLatin1String(kind)).arg(max).arg(have - max);
        list.erase(list.begin() + max, list.end());
    };
    cap("E1.31", e131, kMaxE131Outputs);
    cap("DDP", ddp, kMaxDdpOutputs);
    for (const Json* list : {&e131, &ddp})
        for (const Json& o : *list)
            r.channelsNeeded = qMax(r.channelsNeeded,
                                    o.at("channel_start").get<qint64>() + o.at("channel_count").get<qint64>());
    r.e131Count = int(e131.size());
    r.ddpCount = int(ddp.size());
    r.doc = Json::object();
    r.doc["e131"] = std::move(e131);
    r.doc["ddp"] = std::move(ddp);
    return r;
}
