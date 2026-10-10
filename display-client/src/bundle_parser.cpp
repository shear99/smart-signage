#include "bundle_parser.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <limits>

namespace signage::display {
namespace {

constexpr qsizetype kMaxActiveBytes = 64 * 1024;
constexpr qsizetype kMaxDocumentBytes = 4 * 1024 * 1024;
constexpr int kMaxPlaylistItems = 1'000;
constexpr int kMaxScheduleItems = 5'000;
constexpr int kMaxSubtitleMessages = 1'000;
constexpr int kMaxTextLength = 8'192;

struct ObjectResult {
    QJsonObject object;
    QString error;

    [[nodiscard]] bool isValid() const
    {
        return error.isEmpty();
    }
};

ObjectResult readObject(const QString &path, const qsizetype maxBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {{}, QStringLiteral("cannot open %1").arg(path)};
    }
    if (file.size() <= 0 || file.size() > maxBytes) {
        return {
            {},
            QStringLiteral("invalid JSON document size: %1").arg(path),
        };
    }

    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size()) {
        return {{}, QStringLiteral("short read: %1").arg(path)};
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (
        parseError.error != QJsonParseError::NoError
        || !document.isObject()
    ) {
        return {
            {},
            QStringLiteral("invalid JSON object %1: %2")
                .arg(path, parseError.errorString()),
        };
    }
    return {document.object(), {}};
}

bool integerValue(
    const QJsonObject &object,
    const QString &key,
    int minimum,
    int maximum,
    int *output,
    QString *error
)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        *error = QStringLiteral("%1 must be an integer").arg(key);
        return false;
    }
    const double number = value.toDouble();
    if (
        !std::isfinite(number) || std::floor(number) != number
        || number < minimum || number > maximum
    ) {
        *error = QStringLiteral("%1 is out of range").arg(key);
        return false;
    }
    *output = static_cast<int>(number);
    return true;
}

bool schemaVersionOne(const QJsonObject &object, QString *error)
{
    int version = 0;
    if (!integerValue(
            object,
            QStringLiteral("schemaVersion"),
            1,
            1,
            &version,
            error
        )) {
        return false;
    }
    return version == 1;
}

bool requiredString(
    const QJsonObject &object,
    const QString &key,
    QString *output,
    QString *error,
    const bool allowEmpty = false
)
{
    const QJsonValue value = object.value(key);
    if (!value.isString()) {
        *error = QStringLiteral("%1 must be a string").arg(key);
        return false;
    }
    const QString text = value.toString();
    if ((!allowEmpty && text.isEmpty()) || text.size() > kMaxTextLength) {
        *error = QStringLiteral("%1 has an invalid length").arg(key);
        return false;
    }
    *output = text;
    return true;
}

bool isBundleId(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$)")
    );
    return pattern.match(value).hasMatch();
}

bool isContainedPath(const QString &root, const QString &candidate)
{
    const QString cleanRoot = QDir::cleanPath(root);
    const QString cleanCandidate = QDir::cleanPath(candidate);
    return cleanCandidate == cleanRoot
        || cleanCandidate.startsWith(cleanRoot + QDir::separator());
}

bool validateRegularFile(
    const QString &releaseRoot,
    const QString &relativePath,
    QString *absolutePath,
    QString *error
)
{
    if (
        relativePath.isEmpty() || QDir::isAbsolutePath(relativePath)
        || QDir::cleanPath(relativePath) != relativePath
        || relativePath == QStringLiteral("..")
        || relativePath.startsWith(QStringLiteral("../"))
    ) {
        *error = QStringLiteral("unsafe relative path: %1").arg(relativePath);
        return false;
    }

    const QString candidate = QDir(releaseRoot).filePath(relativePath);
    const QFileInfo info(candidate);
    if (!info.isFile() || info.isSymLink()) {
        *error = QStringLiteral("required regular file is missing: %1")
                     .arg(relativePath);
        return false;
    }
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !isContainedPath(releaseRoot, canonical)) {
        *error = QStringLiteral("file escapes release root: %1")
                     .arg(relativePath);
        return false;
    }
    if (absolutePath != nullptr) {
        *absolutePath = canonical;
    }
    return true;
}

bool parseInsets(
    const QJsonObject &object,
    EdgeInsets *insets,
    QString *error
)
{
    const QJsonValue value = object.value(QStringLiteral("safeInsets"));
    if (!value.isObject()) {
        *error = QStringLiteral("safeInsets must be an object");
        return false;
    }

    const QJsonObject values = value.toObject();
    const auto readInset = [&](const QString &name, qreal *output) {
        const QJsonValue inset = values.value(name);
        if (!inset.isDouble()) {
            *error = QStringLiteral("safeInsets.%1 must be a number").arg(name);
            return false;
        }
        const double number = inset.toDouble();
        if (!std::isfinite(number) || number < 0.0 || number > 0.1) {
            *error = QStringLiteral("safeInsets.%1 is out of range").arg(name);
            return false;
        }
        *output = number;
        return true;
    };

    return readInset(QStringLiteral("top"), &insets->top)
        && readInset(QStringLiteral("right"), &insets->right)
        && readInset(QStringLiteral("bottom"), &insets->bottom)
        && readInset(QStringLiteral("left"), &insets->left);
}

bool parseDisplay(
    const QString &path,
    BundleSnapshot *snapshot,
    QString *error
)
{
    const ObjectResult result = readObject(path, kMaxDocumentBytes);
    if (!result.isValid()) {
        *error = result.error;
        return false;
    }
    if (!schemaVersionOne(result.object, error)) {
        return false;
    }

    QString mode;
    if (!requiredString(
            result.object,
            QStringLiteral("layoutMode"),
            &mode,
            error
        )) {
        return false;
    }
    if (mode == QStringLiteral("dashboard")) {
        snapshot->displayMode = DisplayMode::Dashboard;
    } else if (mode == QStringLiteral("fullscreen_media")) {
        snapshot->displayMode = DisplayMode::FullscreenMedia;
    } else {
        *error = QStringLiteral("unsupported display mode: %1").arg(mode);
        return false;
    }

    QString mediaFit;
    if (!requiredString(
            result.object,
            QStringLiteral("defaultMediaFit"),
            &mediaFit,
            error
        )) {
        return false;
    }
    if (mediaFit == QStringLiteral("fit")) {
        snapshot->defaultMediaFit = MediaFit::Fit;
    } else if (mediaFit == QStringLiteral("crop")) {
        snapshot->defaultMediaFit = MediaFit::Crop;
    } else {
        *error = QStringLiteral("defaultMediaFit must be fit or crop");
        return false;
    }

    const QJsonValue audioValue = result.object.value(QStringLiteral("audio"));
    if (!audioValue.isObject()) {
        *error = QStringLiteral("display audio must be an object");
        return false;
    }
    const QJsonObject audio = audioValue.toObject();
    const QJsonValue muted = audio.value(QStringLiteral("muted"));
    const QJsonValue volume = audio.value(QStringLiteral("volume"));
    if (!muted.isBool() || !volume.isDouble()) {
        *error = QStringLiteral("display audio requires muted and volume");
        return false;
    }
    const double defaultVolume = volume.toDouble();
    if (
        !std::isfinite(defaultVolume)
        || defaultVolume < 0.0 || defaultVolume > 1.0
    ) {
        *error = QStringLiteral("display audio volume is out of range");
        return false;
    }
    snapshot->defaultMuted = muted.toBool();
    snapshot->defaultVolume = defaultVolume;

    return parseInsets(result.object, &snapshot->safeInsets, error);
}

bool parsePlaylist(
    const QString &path,
    const QString &releaseRoot,
    BundleSnapshot *snapshot,
    QString *error
)
{
    const ObjectResult result = readObject(path, kMaxDocumentBytes);
    if (!result.isValid()) {
        *error = result.error;
        return false;
    }
    if (!schemaVersionOne(result.object, error)) {
        return false;
    }

    QString repeat;
    if (!requiredString(
            result.object,
            QStringLiteral("repeat"),
            &repeat,
            error
        ) || repeat != QStringLiteral("all")) {
        *error = QStringLiteral("playlist repeat must be all");
        return false;
    }

    const QJsonValue itemsValue = result.object.value(QStringLiteral("items"));
    if (!itemsValue.isArray()) {
        *error = QStringLiteral("playlist items must be an array");
        return false;
    }
    const QJsonArray items = itemsValue.toArray();
    if (items.size() > kMaxPlaylistItems) {
        *error = QStringLiteral("playlist has too many items");
        return false;
    }

    QSet<QString> ids;
    QSet<int> orders;
    QSet<QString> paths;
    QVector<MediaItem> parsed;
    parsed.reserve(items.size());
    for (const QJsonValue &itemValue : items) {
        if (!itemValue.isObject()) {
            *error = QStringLiteral("playlist item must be an object");
            return false;
        }
        const QJsonObject itemObject = itemValue.toObject();
        MediaItem item;
        QString kind;
        QString relativePath;
        if (
            !requiredString(
                itemObject,
                QStringLiteral("id"),
                &item.id,
                error
            )
            || !integerValue(
                itemObject,
                QStringLiteral("order"),
                0,
                10'000,
                &item.order,
                error
            )
            || !requiredString(
                itemObject,
                QStringLiteral("kind"),
                &kind,
                error
            )
            || !requiredString(
                itemObject,
                QStringLiteral("path"),
                &relativePath,
                error
            )
        ) {
            return false;
        }
        if (kind != QStringLiteral("video")) {
            *error = QStringLiteral("playlist kind must be video");
            return false;
        }
        const QString foldedPath = relativePath.toCaseFolded();
        if (
            ids.contains(item.id) || orders.contains(item.order)
            || paths.contains(foldedPath) || !isBundleId(item.id)
        ) {
            *error = QStringLiteral(
                "playlist id/order/path must be unique and safe"
            );
            return false;
        }
        if (!relativePath.startsWith(QStringLiteral("media/"))) {
            *error = QStringLiteral("playlist media must be under media/");
            return false;
        }
        ids.insert(item.id);
        orders.insert(item.order);
        paths.insert(foldedPath);

        if (!validateRegularFile(
                releaseRoot,
                relativePath,
                &item.absolutePath,
                error
            )) {
            return false;
        }

        const QJsonValue fit = itemObject.value(QStringLiteral("fit"));
        if (!fit.isUndefined() && !fit.isNull()) {
            if (!fit.isString()) {
                *error = QStringLiteral("playlist fit must be fit, crop, or null");
                return false;
            }
            if (fit.toString() == QStringLiteral("fit")) {
                item.fit = MediaFit::Fit;
            } else if (fit.toString() == QStringLiteral("crop")) {
                item.fit = MediaFit::Crop;
            } else {
                *error = QStringLiteral("playlist fit must be fit or crop");
                return false;
            }
        }

        const QJsonValue muted = itemObject.value(QStringLiteral("muted"));
        if (!muted.isUndefined() && !muted.isNull()) {
            if (!muted.isBool()) {
                *error = QStringLiteral("playlist muted must be boolean or null");
                return false;
            }
            item.muted = muted.toBool();
        }

        const QJsonValue volume = itemObject.value(QStringLiteral("volume"));
        if (!volume.isUndefined() && !volume.isNull()) {
            if (!volume.isDouble()) {
                *error = QStringLiteral("playlist volume must be numeric or null");
                return false;
            }
            const double level = volume.toDouble();
            if (!std::isfinite(level) || level < 0.0 || level > 1.0) {
                *error = QStringLiteral("playlist volume is out of range");
                return false;
            }
            item.volume = level;
        }
        parsed.push_back(item);
    }

    std::sort(
        parsed.begin(),
        parsed.end(),
        [](const MediaItem &left, const MediaItem &right) {
            return left.order < right.order;
        }
    );
    snapshot->playlist = parsed;
    return true;
}

bool parseSchedule(
    const QString &path,
    BundleSnapshot *snapshot,
    QString *error
)
{
    const ObjectResult result = readObject(path, kMaxDocumentBytes);
    if (!result.isValid()) {
        *error = result.error;
        return false;
    }
    if (!schemaVersionOne(result.object, error)) {
        return false;
    }

    QString timezone;
    if (!requiredString(
            result.object,
            QStringLiteral("timezone"),
            &timezone,
            error
        ) || timezone != QStringLiteral("Asia/Seoul")) {
        *error = QStringLiteral("schedule timezone must be Asia/Seoul");
        return false;
    }

    const QJsonValue eventsValue = result.object.value(QStringLiteral("events"));
    if (!eventsValue.isArray()) {
        *error = QStringLiteral("schedule events must be an array");
        return false;
    }
    const QJsonArray events = eventsValue.toArray();
    if (events.size() > kMaxScheduleItems) {
        *error = QStringLiteral("schedule has too many events");
        return false;
    }

    QSet<QString> ids;
    QVector<ScheduleItem> parsedItems;
    parsedItems.reserve(events.size());
    for (const QJsonValue &eventValue : events) {
        if (!eventValue.isObject()) {
            *error = QStringLiteral("schedule event must be an object");
            return false;
        }
        const QJsonObject itemObject = eventValue.toObject();
        ScheduleItem item;
        QString startTime;
        QString endTime;
        if (
            !requiredString(
                itemObject,
                QStringLiteral("id"),
                &item.id,
                error
            )
            || !requiredString(
                itemObject,
                QStringLiteral("title"),
                &item.title,
                error
            )
            || !requiredString(
                itemObject,
                QStringLiteral("location"),
                &item.location,
                error
            )
            || !requiredString(
                itemObject,
                QStringLiteral("startTime"),
                &startTime,
                error
            )
            || !requiredString(
                itemObject,
                QStringLiteral("endTime"),
                &endTime,
                error
            )
        ) {
            return false;
        }
        if (ids.contains(item.id) || !isBundleId(item.id)) {
            *error = QStringLiteral("schedule ids must be unique and safe");
            return false;
        }
        ids.insert(item.id);

        static const QRegularExpression timePattern(
            QStringLiteral(R"(^(?:[01]\d|2[0-3]):[0-5]\d$)")
        );
        if (
            !timePattern.match(startTime).hasMatch()
            || !timePattern.match(endTime).hasMatch()
        ) {
            *error = QStringLiteral("schedule times must use HH:mm");
            return false;
        }
        item.startTime = QTime::fromString(startTime, QStringLiteral("HH:mm"));
        item.endTime = QTime::fromString(endTime, QStringLiteral("HH:mm"));
        if (!item.startTime.isValid() || !item.endTime.isValid()) {
            *error = QStringLiteral("invalid schedule time");
            return false;
        }

        const QJsonValue daysValue =
            itemObject.value(QStringLiteral("daysOfWeek"));
        const QJsonValue dateValue = itemObject.value(QStringLiteral("date"));
        const bool hasDays = !daysValue.isUndefined() && !daysValue.isNull();
        const bool hasDate = !dateValue.isUndefined() && !dateValue.isNull();
        if (hasDays == hasDate) {
            *error = QStringLiteral(
                "schedule requires exactly one of daysOfWeek or date"
            );
            return false;
        }
        if (hasDays) {
            if (!daysValue.isArray()) {
                *error = QStringLiteral("daysOfWeek must be an array or null");
                return false;
            }
            const QJsonArray days = daysValue.toArray();
            if (days.isEmpty() || days.size() > 7) {
                *error = QStringLiteral("daysOfWeek has an invalid length");
                return false;
            }
            QSet<int> uniqueDays;
            for (const QJsonValue &dayValue : days) {
                if (!dayValue.isDouble()) {
                    *error = QStringLiteral("daysOfWeek values must be integers");
                    return false;
                }
                const double dayNumber = dayValue.toDouble();
                if (
                    std::floor(dayNumber) != dayNumber
                    || dayNumber < 1 || dayNumber > 7
                ) {
                    *error = QStringLiteral(
                        "daysOfWeek uses ISO values 1 through 7"
                    );
                    return false;
                }
                const int day = static_cast<int>(dayNumber);
                if (uniqueDays.contains(day)) {
                    *error = QStringLiteral("daysOfWeek contains duplicates");
                    return false;
                }
                uniqueDays.insert(day);
                item.daysOfWeek.push_back(day);
            }
            std::sort(item.daysOfWeek.begin(), item.daysOfWeek.end());
        } else {
            if (!dateValue.isString()) {
                *error = QStringLiteral("schedule date must be ISO date or null");
                return false;
            }
            const QDate date = QDate::fromString(
                dateValue.toString(),
                Qt::ISODate
            );
            if (!date.isValid()) {
                *error = QStringLiteral("invalid schedule date");
                return false;
            }
            item.date = date;
        }

        const QJsonValue nextDay =
            itemObject.value(QStringLiteral("endsNextDay"));
        if (!nextDay.isBool()) {
            *error = QStringLiteral("endsNextDay must be boolean");
            return false;
        }
        item.endsNextDay = nextDay.toBool();
        if (item.startTime == item.endTime) {
            *error = QStringLiteral("schedule start and end must differ");
            return false;
        }
        if (
            (item.endTime < item.startTime && !item.endsNextDay)
            || (item.endTime > item.startTime && item.endsNextDay)
        ) {
            *error = QStringLiteral("endsNextDay does not match time interval");
            return false;
        }

        if (!integerValue(
                itemObject,
                QStringLiteral("priority"),
                -1'000,
                1'000,
                &item.priority,
                error
            )) {
            return false;
        }
        parsedItems.push_back(item);
    }
    snapshot->schedule = parsedItems;
    return true;
}

bool parseSubtitle(
    const QString &path,
    BundleSnapshot *snapshot,
    QString *error
)
{
    const ObjectResult result = readObject(path, kMaxDocumentBytes);
    if (!result.isValid()) {
        *error = result.error;
        return false;
    }
    int schemaVersion = 0;
    if (!integerValue(
            result.object,
            QStringLiteral("schemaVersion"),
            1,
            2,
            &schemaVersion,
            error
        )) {
        return false;
    }

    const QJsonValue messagesValue =
        result.object.value(QStringLiteral("messages"));
    if (!messagesValue.isArray()) {
        *error = QStringLiteral("subtitle messages must be an array");
        return false;
    }
    const QJsonArray messages = messagesValue.toArray();
    if (messages.size() > kMaxSubtitleMessages) {
        *error = QStringLiteral("subtitle has too many messages");
        return false;
    }

    QSet<QString> ids;
    QVector<SubtitleMessage> parsed;
    parsed.reserve(messages.size());
    for (const QJsonValue &messageValue : messages) {
        if (!messageValue.isObject()) {
            *error = QStringLiteral("subtitle message must be an object");
            return false;
        }
        const QJsonObject object = messageValue.toObject();
        SubtitleMessage message;
        if (
            !requiredString(
                object,
                QStringLiteral("id"),
                &message.id,
                error
            )
            || !integerValue(
                object,
                QStringLiteral("durationMs"),
                1'000,
                60'000,
                &message.durationMs,
                error
            )
        ) {
            return false;
        }

        if (object.contains(QStringLiteral("effect"))) {
            if (!requiredString(
                    object,
                    QStringLiteral("effect"),
                    &message.effect,
                    error
                )
                || !QSet<QString>{
                    QStringLiteral("fade"),
                    QStringLiteral("slide_left"),
                    QStringLiteral("slide_up"),
                    QStringLiteral("soft_zoom"),
                }.contains(message.effect)
            ) {
                *error = QStringLiteral("subtitle effect is invalid");
                return false;
            }
        }
        if (
            object.contains(QStringLiteral("fontSizePx"))
            && !integerValue(
                object,
                QStringLiteral("fontSizePx"),
                24,
                96,
                &message.fontSizePx,
                error
            )
        ) {
            return false;
        }
        if (
            object.contains(QStringLiteral("transitionMs"))
            && !integerValue(
                object,
                QStringLiteral("transitionMs"),
                150,
                3'000,
                &message.transitionMs,
                error
            )
        ) {
            return false;
        }

        if (schemaVersion == 1) {
            if (!requiredString(
                    object,
                    QStringLiteral("text"),
                    &message.text,
                    error,
                    true
                )) {
                return false;
            }
            if (message.text.size() > 2'000) {
                *error = QStringLiteral("subtitle text is too long");
                return false;
            }
            message.richText = message.text.toHtmlEscaped();
            message.richText.replace(
                QChar::LineFeed,
                QStringLiteral("<br/>")
            );
        } else {
            const QJsonValue linesValue =
                object.value(QStringLiteral("lines"));
            if (!linesValue.isArray()) {
                *error = QStringLiteral("subtitle lines must be an array");
                return false;
            }
            const QJsonArray lines = linesValue.toArray();
            if (lines.isEmpty() || lines.size() > 2) {
                *error = QStringLiteral("subtitle must contain one or two lines");
                return false;
            }
            QStringList plainLines;
            QStringList richLines;
            int totalCharacters = 0;
            for (const QJsonValue &lineValue : lines) {
                if (!lineValue.isObject()) {
                    *error = QStringLiteral("subtitle line must be an object");
                    return false;
                }
                const QJsonValue spansValue =
                    lineValue.toObject().value(QStringLiteral("spans"));
                if (!spansValue.isArray()) {
                    *error = QStringLiteral("subtitle spans must be an array");
                    return false;
                }
                const QJsonArray spans = spansValue.toArray();
                if (spans.isEmpty() || spans.size() > 200) {
                    *error = QStringLiteral(
                        "subtitle line must contain 1 through 200 spans"
                    );
                    return false;
                }
                QString plainLine;
                QString richLine;
                for (const QJsonValue &spanValue : spans) {
                    if (!spanValue.isObject()) {
                        *error = QStringLiteral(
                            "subtitle span must be an object"
                        );
                        return false;
                    }
                    const QJsonObject span = spanValue.toObject();
                    QString text;
                    QString size;
                    if (
                        !requiredString(
                            span,
                            QStringLiteral("text"),
                            &text,
                            error
                        )
                        || !requiredString(
                            span,
                            QStringLiteral("size"),
                            &size,
                            error
                        )
                    ) {
                        return false;
                    }
                    if (
                        text.size() > 2'000
                        || text.contains(QChar::LineFeed)
                        || !QSet<QString>{
                            QStringLiteral("small"),
                            QStringLiteral("normal"),
                            QStringLiteral("large"),
                        }.contains(size)
                    ) {
                        *error = QStringLiteral(
                            "subtitle span text or size is invalid"
                        );
                        return false;
                    }
                    const QJsonValue marksValue =
                        span.value(QStringLiteral("marks"));
                    if (!marksValue.isArray()) {
                        *error = QStringLiteral(
                            "subtitle marks must be an array"
                        );
                        return false;
                    }
                    const QJsonArray marks = marksValue.toArray();
                    if (marks.size() > 4) {
                        *error = QStringLiteral(
                            "subtitle has too many marks"
                        );
                        return false;
                    }
                    QSet<QString> markSet;
                    for (const QJsonValue &markValue : marks) {
                        if (!markValue.isString()) {
                            *error = QStringLiteral(
                                "subtitle mark must be a string"
                            );
                            return false;
                        }
                        const QString mark = markValue.toString();
                        if (
                            markSet.contains(mark)
                            || !QSet<QString>{
                                QStringLiteral("bold"),
                                QStringLiteral("italic"),
                                QStringLiteral("underline"),
                                QStringLiteral("strike"),
                            }.contains(mark)
                        ) {
                            *error = QStringLiteral(
                                "subtitle mark is invalid or duplicated"
                            );
                            return false;
                        }
                        markSet.insert(mark);
                    }

                    plainLine += text;
                    totalCharacters += text.size();
                    QString formatted = text.toHtmlEscaped();
                    if (markSet.contains(QStringLiteral("bold"))) {
                        formatted = QStringLiteral("<b>%1</b>").arg(formatted);
                    }
                    if (markSet.contains(QStringLiteral("italic"))) {
                        formatted = QStringLiteral("<i>%1</i>").arg(formatted);
                    }
                    if (markSet.contains(QStringLiteral("underline"))) {
                        formatted = QStringLiteral("<u>%1</u>").arg(formatted);
                    }
                    if (markSet.contains(QStringLiteral("strike"))) {
                        formatted = QStringLiteral("<s>%1</s>").arg(formatted);
                    }
                    const int percent = size == QStringLiteral("small")
                        ? 80
                        : size == QStringLiteral("large") ? 115 : 100;
                    richLine += QStringLiteral(
                        "<span style=\"font-size:%1%%\">%2</span>"
                    ).arg(percent).arg(formatted);
                }
                plainLines.push_back(plainLine);
                richLines.push_back(richLine);
            }
            if (totalCharacters > 80) {
                *error = QStringLiteral(
                    "formatted subtitle text is too long"
                );
                return false;
            }
            message.text = plainLines.join(QChar::LineFeed);
            message.richText = richLines.join(QStringLiteral("<br/>"));
        }
        if (ids.contains(message.id) || !isBundleId(message.id)) {
            *error = QStringLiteral("subtitle ids must be unique and safe");
            return false;
        }
        ids.insert(message.id);
        parsed.push_back(message);
    }
    snapshot->subtitles = parsed;
    return true;
}

bool parseActivation(
    const QJsonObject &bundle,
    BundleSnapshot *snapshot,
    QString *error
)
{
    const QJsonValue activationValue =
        bundle.value(QStringLiteral("activation"));
    if (!activationValue.isObject()) {
        *error = QStringLiteral("bundle activation must be an object");
        return false;
    }
    const QJsonObject activation = activationValue.toObject();
    QString settings;
    QString playlist;
    QString emergency;
    if (
        !requiredString(
            activation,
            QStringLiteral("settings"),
            &settings,
            error
        )
        || !requiredString(
            activation,
            QStringLiteral("playlist"),
            &playlist,
            error
        )
        || !requiredString(
            activation,
            QStringLiteral("emergency"),
            &emergency,
            error
        )
    ) {
        return false;
    }
    if (
        settings != QStringLiteral("immediate")
        || emergency != QStringLiteral("immediate")
        || playlist != QStringLiteral("after_current_item")
    ) {
        *error = QStringLiteral("unsupported bundle activation policy");
        return false;
    }
    snapshot->playlistActivation = ActivationPolicy::AfterCurrentItem;
    return true;
}

bool validateBundleDocument(
    const QJsonObject &documents,
    const QString &key,
    const QString &expectedName,
    QString *error
)
{
    QString value;
    if (!requiredString(documents, key, &value, error)) {
        return false;
    }
    if (value != expectedName) {
        *error = QStringLiteral("%1 document must be %2")
                     .arg(key, expectedName);
        return false;
    }
    return true;
}

QString sha256File(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot hash manifest");
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        *error = QStringLiteral("cannot read manifest for hashing");
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool explicitZonedDateTime(const QString &text)
{
    static const QRegularExpression explicitZone(
        QStringLiteral(R"((Z|[+-]\d{2}:\d{2})$)")
    );
    if (!explicitZone.match(text).hasMatch()) {
        return false;
    }
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(text, Qt::ISODate);
    }
    return parsed.isValid() && parsed.timeSpec() != Qt::LocalTime;
}

}  // namespace

BundleParseResult BundleParser::loadActive(const QString &contentRoot)
{
    QString root = QFileInfo(contentRoot).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        return {
            std::nullopt,
            QStringLiteral("content root is not a readable directory"),
        };
    }

    const QString activePath = QDir(root).filePath(QStringLiteral("active.json"));
    const ObjectResult active = readObject(activePath, kMaxActiveBytes);
    if (!active.isValid()) {
        return {std::nullopt, active.error};
    }
    QString error;
    if (!schemaVersionOne(active.object, &error)) {
        return {std::nullopt, QStringLiteral("active.json: %1").arg(error)};
    }

    QString bundleId;
    QString relativeReleasePath;
    QString manifestSha256;
    QString activatedAt;
    int revision = 0;
    int generation = 0;
    if (
        !requiredString(
            active.object,
            QStringLiteral("bundleId"),
            &bundleId,
            &error
        )
        || !requiredString(
            active.object,
            QStringLiteral("path"),
            &relativeReleasePath,
            &error
        )
        || !integerValue(
            active.object,
            QStringLiteral("revision"),
            1,
            std::numeric_limits<int>::max(),
            &revision,
            &error
        )
        || !integerValue(
            active.object,
            QStringLiteral("generation"),
            1,
            std::numeric_limits<int>::max(),
            &generation,
            &error
        )
        || !requiredString(
            active.object,
            QStringLiteral("manifestSha256"),
            &manifestSha256,
            &error
        )
        || !requiredString(
            active.object,
            QStringLiteral("activatedAt"),
            &activatedAt,
            &error
        )
    ) {
        return {std::nullopt, QStringLiteral("active.json: %1").arg(error)};
    }
    static const QRegularExpression shaPattern(
        QStringLiteral(R"(^[0-9a-f]{64}$)")
    );
    const QString expectedBundleId = QStringLiteral("r%1-%2")
        .arg(revision)
        .arg(manifestSha256.first(16));
    if (
        active.object.size() != 7
        || !shaPattern.match(manifestSha256).hasMatch()
        || bundleId != expectedBundleId
        || !explicitZonedDateTime(activatedAt)
    ) {
        return {
            std::nullopt,
            QStringLiteral("active.json identity is invalid"),
        };
    }

    const QString expectedPath =
        QStringLiteral("releases/%1").arg(bundleId);
    if (relativeReleasePath != expectedPath) {
        return {
            std::nullopt,
            QStringLiteral("active.json path must equal %1").arg(expectedPath),
        };
    }

    const QString releaseCandidate = QDir(root).filePath(relativeReleasePath);
    const QFileInfo releaseInfo(releaseCandidate);
    const QString releaseRoot = releaseInfo.canonicalFilePath();
    if (
        !releaseInfo.isDir() || releaseInfo.isSymLink()
        || releaseRoot.isEmpty() || !isContainedPath(root, releaseRoot)
    ) {
        return {
            std::nullopt,
            QStringLiteral("active release is missing or escapes content root"),
        };
    }

    QString manifestPath;
    QString completePath;
    if (!validateRegularFile(
            releaseRoot,
            QStringLiteral(".manifest.json"),
            &manifestPath,
            &error
        )
        || !validateRegularFile(
            releaseRoot,
            QStringLiteral(".complete"),
            &completePath,
            &error
        )) {
        return {std::nullopt, error};
    }
    if (sha256File(manifestPath, &error) != manifestSha256) {
        return {
            std::nullopt,
            error.isEmpty()
                ? QStringLiteral("manifest SHA-256 does not match active.json")
                : error,
        };
    }
    const ObjectResult complete = readObject(completePath, kMaxActiveBytes);
    if (!complete.isValid()) {
        return {std::nullopt, complete.error};
    }
    QString completeBundleId;
    QString completeManifestSha;
    QString completedAt;
    int completeRevision = 0;
    if (
        !schemaVersionOne(complete.object, &error)
        || !requiredString(
            complete.object,
            QStringLiteral("bundleId"),
            &completeBundleId,
            &error
        )
        || !integerValue(
            complete.object,
            QStringLiteral("revision"),
            1,
            std::numeric_limits<int>::max(),
            &completeRevision,
            &error
        )
        || !requiredString(
            complete.object,
            QStringLiteral("manifestSha256"),
            &completeManifestSha,
            &error
        )
        || !requiredString(
            complete.object,
            QStringLiteral("completedAt"),
            &completedAt,
            &error
        )
        || complete.object.size() != 5
        || completeBundleId != bundleId
        || completeRevision != revision
        || completeManifestSha != manifestSha256
        || !explicitZonedDateTime(completedAt)
    ) {
        return {
            std::nullopt,
            QStringLiteral("release completion identity is invalid"),
        };
    }

    QString bundlePath;
    if (!validateRegularFile(
            releaseRoot,
            QStringLiteral("bundle.json"),
            &bundlePath,
            &error
        )) {
        return {std::nullopt, error};
    }
    const ObjectResult bundle = readObject(bundlePath, kMaxDocumentBytes);
    if (!bundle.isValid()) {
        return {std::nullopt, bundle.error};
    }
    if (!schemaVersionOne(bundle.object, &error)) {
        return {std::nullopt, QStringLiteral("bundle.json: %1").arg(error)};
    }
    if (bundle.object.size() != 6) {
        return {
            std::nullopt,
            QStringLiteral("bundle.json has unexpected fields"),
        };
    }

    if (
        !validateBundleDocument(
            bundle.object,
            QStringLiteral("display"),
            QStringLiteral("display.json"),
            &error
        )
        || !validateBundleDocument(
            bundle.object,
            QStringLiteral("playlist"),
            QStringLiteral("playlist.json"),
            &error
        )
        || !validateBundleDocument(
            bundle.object,
            QStringLiteral("schedule"),
            QStringLiteral("schedule.json"),
            &error
        )
        || !validateBundleDocument(
            bundle.object,
            QStringLiteral("subtitle"),
            QStringLiteral("subtitle.json"),
            &error
        )
    ) {
        return {std::nullopt, QStringLiteral("bundle.json: %1").arg(error)};
    }

    QString displayPath;
    QString playlistPath;
    QString schedulePath;
    QString subtitlePath;
    if (
        !validateRegularFile(
            releaseRoot,
            QStringLiteral("display.json"),
            &displayPath,
            &error
        )
        || !validateRegularFile(
            releaseRoot,
            QStringLiteral("playlist.json"),
            &playlistPath,
            &error
        )
        || !validateRegularFile(
            releaseRoot,
            QStringLiteral("schedule.json"),
            &schedulePath,
            &error
        )
        || !validateRegularFile(
            releaseRoot,
            QStringLiteral("subtitle.json"),
            &subtitlePath,
            &error
        )
    ) {
        return {std::nullopt, error};
    }

    BundleSnapshot snapshot;
    snapshot.bundleId = bundleId;
    snapshot.releaseRoot = releaseRoot;
    if (
        !parseActivation(bundle.object, &snapshot, &error)
        || !parseDisplay(displayPath, &snapshot, &error)
        || !parsePlaylist(playlistPath, releaseRoot, &snapshot, &error)
        || !parseSchedule(schedulePath, &snapshot, &error)
        || !parseSubtitle(subtitlePath, &snapshot, &error)
    ) {
        return {std::nullopt, error};
    }

    return {snapshot, {}};
}

}  // namespace signage::display
