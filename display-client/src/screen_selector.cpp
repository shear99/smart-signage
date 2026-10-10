#include "screen_selector.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace signage::display {
namespace {

QString readTrimmed(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

bool validEdid(const QByteArray &bytes)
{
    static const QByteArray header =
        QByteArray::fromHex("00ffffffffffff00");
    if (
        bytes.size() < 128 || bytes.size() % 128 != 0
        || bytes.first(header.size()) != header
    ) {
        return false;
    }
    for (qsizetype block = 0; block < bytes.size(); block += 128) {
        quint8 checksum = 0;
        for (qsizetype index = block; index < block + 128; ++index) {
            checksum = static_cast<quint8>(
                checksum + static_cast<quint8>(bytes[index])
            );
        }
        if (checksum != 0) {
            return false;
        }
    }
    return true;
}

QString descriptorText(
    const QByteArray &edid,
    const quint8 descriptorType
)
{
    const qsizetype descriptorLimit = std::min<qsizetype>(edid.size(), 126);
    for (int offset = 54; offset + 18 <= descriptorLimit; offset += 18) {
        if (
            static_cast<quint8>(edid[offset]) == 0
            && static_cast<quint8>(edid[offset + 1]) == 0
            && static_cast<quint8>(edid[offset + 3]) == descriptorType
        ) {
            QByteArray text = edid.mid(offset + 5, 13);
            text.replace('\n', ' ');
            text.replace('\r', ' ');
            text.replace('\0', ' ');
            return QString::fromLatin1(text).trimmed();
        }
    }
    return {};
}

PhysicalOutput parseOutput(
    const QString &connectorName,
    const QByteArray &edid
)
{
    PhysicalOutput output;
    output.connectorName = connectorName;
    output.edidSha256 = QString::fromLatin1(
        QCryptographicHash::hash(edid, QCryptographicHash::Sha256).toHex()
    );

    const quint16 manufacturerCode =
        (static_cast<quint8>(edid[8]) << 8)
        | static_cast<quint8>(edid[9]);
    QString manufacturer;
    for (int shift : {10, 5, 0}) {
        const int value = (manufacturerCode >> shift) & 0x1f;
        if (value < 1 || value > 26) {
            manufacturer.clear();
            break;
        }
        manufacturer.append(QChar('A' + value - 1));
    }
    output.manufacturer = manufacturer;

    output.model = descriptorText(edid, 0xfc);
    if (output.model.isEmpty()) {
        const quint16 productCode =
            static_cast<quint8>(edid[10])
            | (static_cast<quint8>(edid[11]) << 8);
        output.model = QStringLiteral("%1")
                           .arg(productCode, 4, 16, QLatin1Char('0'))
                           .toUpper();
    }

    output.serial = descriptorText(edid, 0xff);
    if (output.serial.isEmpty()) {
        const quint32 serial =
            static_cast<quint8>(edid[12])
            | (static_cast<quint8>(edid[13]) << 8)
            | (static_cast<quint8>(edid[14]) << 16)
            | (static_cast<quint8>(edid[15]) << 24);
        if (serial != 0) {
            output.serial = QString::number(serial);
        }
    }
    return output;
}

QString normalized(const QString &value)
{
    QString result = value.simplified().toLower();
    result.remove(QRegularExpression(QStringLiteral("[^a-z0-9]+")));
    return result;
}

QString connectorKey(QString value)
{
    value = value.toLower();
    value.remove(QRegularExpression(QStringLiteral("^card\\d+-")));
    value.replace(QStringLiteral("hdmi-a-"), QStringLiteral("hdmi-"));
    value.remove(QRegularExpression(QStringLiteral("[^a-z0-9]+")));
    return value;
}

int connectorRank(const QString &name)
{
    if (name == QStringLiteral("HDMI-A-1")) {
        return 0;
    }
    if (name == QStringLiteral("HDMI-A-2")) {
        return 1;
    }
    return 2;
}

}  // namespace

QVector<PhysicalOutput> DrmOutputScanner::scan(const QString &drmRoot)
{
    const QDir root(drmRoot);
    if (!root.exists()) {
        return {};
    }

    static const QRegularExpression directoryPattern(
        QStringLiteral(R"(^card\d+-(.+)$)")
    );
    static const QRegularExpression physicalConnectorPattern(
        QStringLiteral(
            R"(^(HDMI-A|DP|DVI-I|DVI-D|DVI-A|eDP|VGA)-\d+$)"
        )
    );

    QVector<PhysicalOutput> outputs;
    const QFileInfoList entries = root.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name
    );
    for (const QFileInfo &entry : entries) {
        const QRegularExpressionMatch match =
            directoryPattern.match(entry.fileName());
        if (!match.hasMatch()) {
            continue;
        }
        const QString connectorName = match.captured(1);
        if (!physicalConnectorPattern.match(connectorName).hasMatch()) {
            continue;
        }
        if (
            readTrimmed(QDir(entry.filePath()).filePath(QStringLiteral("status")))
            != QStringLiteral("connected")
        ) {
            continue;
        }

        QFile edidFile(
            QDir(entry.filePath()).filePath(QStringLiteral("edid"))
        );
        if (!edidFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray edid = edidFile.readAll();
        if (!validEdid(edid)) {
            continue;
        }
        outputs.push_back(parseOutput(connectorName, edid));
    }
    return outputs;
}

int ScreenSelector::preferredOutputIndex(
    const QVector<PhysicalOutput> &outputs,
    const QString &preferredEdidSha256
)
{
    if (outputs.isEmpty()) {
        return -1;
    }

    QVector<int> indexes;
    indexes.reserve(outputs.size());
    for (int index = 0; index < outputs.size(); ++index) {
        indexes.push_back(index);
    }
    std::stable_sort(
        indexes.begin(),
        indexes.end(),
        [&](const int left, const int right) {
            const bool leftIdentity =
                !preferredEdidSha256.isEmpty()
                && outputs[left].edidSha256.compare(
                    preferredEdidSha256,
                    Qt::CaseInsensitive
                ) == 0;
            const bool rightIdentity =
                !preferredEdidSha256.isEmpty()
                && outputs[right].edidSha256.compare(
                    preferredEdidSha256,
                    Qt::CaseInsensitive
                ) == 0;
            if (leftIdentity != rightIdentity) {
                return leftIdentity;
            }
            const int leftRank = connectorRank(outputs[left].connectorName);
            const int rightRank = connectorRank(outputs[right].connectorName);
            if (leftRank != rightRank) {
                return leftRank < rightRank;
            }
            return outputs[left].connectorName < outputs[right].connectorName;
        }
    );
    return indexes.front();
}

int ScreenSelector::matchingScreenIndex(
    const PhysicalOutput &output,
    const QVector<ScreenDescriptor> &screens
)
{
    const QString outputConnector = connectorKey(output.connectorName);
    for (int index = 0; index < screens.size(); ++index) {
        if (
            !isVirtualScreenName(screens[index].name)
            && connectorKey(screens[index].name) == outputConnector
        ) {
            return index;
        }
    }

    int bestIndex = -1;
    int bestScore = 0;
    int nonVirtualCount = 0;
    int soleNonVirtualIndex = -1;
    for (int index = 0; index < screens.size(); ++index) {
        const ScreenDescriptor &screen = screens[index];
        if (isVirtualScreenName(screen.name)) {
            continue;
        }
        ++nonVirtualCount;
        soleNonVirtualIndex = index;

        int score = 0;
        if (
            !output.serial.isEmpty()
            && normalized(output.serial) == normalized(screen.serial)
        ) {
            score += 8;
        }
        if (
            !output.model.isEmpty()
            && normalized(screen.model).contains(normalized(output.model))
        ) {
            score += 4;
        }
        if (
            !output.manufacturer.isEmpty()
            && normalized(output.manufacturer)
                == normalized(screen.manufacturer)
        ) {
            score += 2;
        }
        if (score > bestScore) {
            bestScore = score;
            bestIndex = index;
        }
    }
    if (bestScore >= 4) {
        return bestIndex;
    }
    if (nonVirtualCount == 1) {
        return soleNonVirtualIndex;
    }
    return -1;
}

bool ScreenSelector::isVirtualScreenName(const QString &name)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            R"((virtual|headless|dummy|writeback|rdp|vnc|xvfb|pi.?connect))"
        ),
        QRegularExpression::CaseInsensitiveOption
    );
    return pattern.match(name).hasMatch();
}

}  // namespace signage::display
