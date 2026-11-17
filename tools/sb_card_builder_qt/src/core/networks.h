// xLights xlights_networks.xml -> the player's outputs.json
#pragma once

#include "core/json.h"

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

constexpr int kMaxE131Outputs = 8;  // MAX_E131_OUTPUTS in firmware config.h
constexpr int kMaxDdpOutputs = 8;   // MAX_DDP_OUTPUTS

struct Controller {
    QString name, protocol, ip;
    QString state = QStringLiteral("Active");  // Active / Inactive / xLights Only
    qint64 start = 0;                           // 0-based absolute start channel
    qint64 channels = 0;
    QVector<QPair<int, int>> universes;         // (universe, size) for E1.31
    bool keepNumbers = true;                    // DDP "Keep Channel Numbers"

    bool usable() const { return protocol == QLatin1String("E131") || protocol == QLatin1String("DDP"); }
    QString universeText() const;
};

// Controllers in channel order. Throws std::runtime_error if the file cannot be read.
QVector<Controller> parseNetworks(const QString& path);

struct OutputsResult {
    Json doc;
    QStringList warnings;
    qint64 channelsNeeded = 0;
    int e131Count = 0, ddpCount = 0;
};

OutputsResult buildOutputs(const QVector<Controller>& controllers);
