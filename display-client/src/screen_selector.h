#pragma once

#include <QString>
#include <QVector>

namespace signage::display {

struct PhysicalOutput {
    QString connectorName;
    QString edidSha256;
    QString manufacturer;
    QString model;
    QString serial;
};

struct ScreenDescriptor {
    QString name;
    QString manufacturer;
    QString model;
    QString serial;
};

class DrmOutputScanner {
public:
    [[nodiscard]] static QVector<PhysicalOutput> scan(
        const QString &drmRoot = QStringLiteral("/sys/class/drm")
    );
};

class ScreenSelector {
public:
    [[nodiscard]] static int preferredOutputIndex(
        const QVector<PhysicalOutput> &outputs,
        const QString &preferredEdidSha256
    );

    [[nodiscard]] static int matchingScreenIndex(
        const PhysicalOutput &output,
        const QVector<ScreenDescriptor> &screens
    );

    [[nodiscard]] static bool isVirtualScreenName(const QString &name);
};

}  // namespace signage::display
