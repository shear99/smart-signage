#include "schedule_projection.h"

#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>

namespace signage::display {
namespace {

bool appliesOnDate(const ScheduleItem &item, const QDate &date)
{
    if (item.date.has_value()) {
        return *item.date == date;
    }
    return item.daysOfWeek.contains(date.dayOfWeek());
}

std::optional<int> floorNumber(const QString &location)
{
    static const QVector<QRegularExpression> patterns{
        QRegularExpression(QStringLiteral(R"((?:^|\D)([234])\s*층(?:\D|$))")),
        QRegularExpression(
            QStringLiteral(R"((?:^|\D)([234])\s*[Ff](?:\D|$))")
        ),
        QRegularExpression(
            QStringLiteral(R"((?:^|\D)[Ff]\s*([234])(?:\D|$))")
        ),
    };
    for (const QRegularExpression &pattern : patterns) {
        const QRegularExpressionMatch match = pattern.match(location);
        if (match.hasMatch()) {
            return match.captured(1).toInt();
        }
    }
    return std::nullopt;
}

struct Occurrence {
    ScheduleItem item;
    QDateTime startsAtUtc;
    QDateTime endsAtUtc;
};

std::optional<Occurrence> occurrenceIncludingEnded(
    const ScheduleItem &item,
    const QDate &date
)
{
    if (!appliesOnDate(item, date)) {
        return std::nullopt;
    }
    const QTimeZone seoul(QByteArrayLiteral("Asia/Seoul"));
    const QDateTime startLocal(date, item.startTime, seoul);
    const QDate endDate = item.endsNextDay ? date.addDays(1) : date;
    const QDateTime endLocal(endDate, item.endTime, seoul);
    if (!startLocal.isValid() || !endLocal.isValid() || endLocal <= startLocal) {
        return std::nullopt;
    }
    return Occurrence{item, startLocal.toUTC(), endLocal.toUTC()};
}

std::optional<ScheduleRow> occurrenceOn(
    const ScheduleItem &item,
    const QDate &date,
    const QDateTime &nowUtc
)
{
    if (!appliesOnDate(item, date)) {
        return std::nullopt;
    }

    const QTimeZone seoul(QByteArrayLiteral("Asia/Seoul"));
    const QDateTime startLocal(date, item.startTime, seoul);
    const QDate endDate = item.endsNextDay ? date.addDays(1) : date;
    const QDateTime endLocal(endDate, item.endTime, seoul);
    if (!startLocal.isValid() || !endLocal.isValid() || endLocal <= startLocal) {
        return std::nullopt;
    }

    const QDateTime startsAtUtc = startLocal.toUTC();
    const QDateTime endsAtUtc = endLocal.toUTC();
    if (endsAtUtc <= nowUtc) {
        return std::nullopt;
    }
    return ScheduleRow{
        item,
        startsAtUtc,
        endsAtUtc,
        startsAtUtc <= nowUtc
            ? ScheduleStatus::Active
            : ScheduleStatus::Upcoming,
    };
}

bool occurrenceOrder(const Occurrence &left, const Occurrence &right)
{
    if (left.startsAtUtc != right.startsAtUtc) {
        return left.startsAtUtc < right.startsAtUtc;
    }
    if (left.item.priority != right.item.priority) {
        return left.item.priority > right.item.priority;
    }
    return left.item.id < right.item.id;
}

FloorScheduleStatus upcomingStatus(
    const QDateTime &startsAtUtc,
    const QDateTime &nowUtc
)
{
    const qint64 seconds = nowUtc.secsTo(startsAtUtc);
    return seconds <= 10 * 60
        ? FloorScheduleStatus::StartingSoon
        : FloorScheduleStatus::Upcoming;
}

bool isFirstOccurrenceStartingToday(
    const Occurrence &candidate,
    const QVector<Occurrence> &occurrences,
    const QDateTime &nowUtc
)
{
    const QTimeZone seoul(QByteArrayLiteral("Asia/Seoul"));
    const QDate today = nowUtc.toTimeZone(seoul).date();
    if (candidate.startsAtUtc.toTimeZone(seoul).date() != today) {
        return false;
    }
    return std::none_of(
        occurrences.cbegin(),
        occurrences.cend(),
        [&](const Occurrence &occurrence) {
            return occurrence.startsAtUtc < candidate.startsAtUtc
                && occurrence.startsAtUtc.toTimeZone(seoul).date() == today;
        }
    );
}

FloorScheduleRow selectFloorRow(
    const int floor,
    QVector<Occurrence> occurrences,
    const QDateTime &nowUtc
)
{
    std::sort(occurrences.begin(), occurrences.end(), occurrenceOrder);

    QVector<Occurrence> active;
    std::optional<Occurrence> previous;
    std::optional<Occurrence> next;
    for (const Occurrence &occurrence : occurrences) {
        if (
            occurrence.startsAtUtc <= nowUtc
            && nowUtc < occurrence.endsAtUtc
        ) {
            active.push_back(occurrence);
        } else if (occurrence.endsAtUtc <= nowUtc) {
            if (
                !previous.has_value()
                || previous->endsAtUtc < occurrence.endsAtUtc
                || (
                    previous->endsAtUtc == occurrence.endsAtUtc
                    && previous->item.priority < occurrence.item.priority
                )
            ) {
                previous = occurrence;
            }
        } else if (nowUtc < occurrence.startsAtUtc && !next.has_value()) {
            next = occurrence;
        }
    }

    if (!active.isEmpty()) {
        std::sort(
            active.begin(),
            active.end(),
            [](const Occurrence &left, const Occurrence &right) {
                if (left.item.priority != right.item.priority) {
                    return left.item.priority > right.item.priority;
                }
                return occurrenceOrder(left, right);
            }
        );
        const Occurrence &selected = active.first();
        return {
            floor,
            selected.item,
            selected.startsAtUtc,
            selected.endsAtUtc,
            FloorScheduleStatus::Active,
        };
    }

    if (
        next.has_value()
        && nowUtc.secsTo(next->startsAtUtc) > 30 * 60
        && isFirstOccurrenceStartingToday(*next, occurrences, nowUtc)
    ) {
        return {
            floor,
            next->item,
            next->startsAtUtc,
            next->endsAtUtc,
            FloorScheduleStatus::Inactive,
        };
    }

    if (previous.has_value()) {
        bool showNext = false;
        if (next.has_value()) {
            const qint64 gapSeconds =
                previous->endsAtUtc.secsTo(next->startsAtUtc);
            showNext =
                gapSeconds <= 30 * 60
                || nowUtc.secsTo(next->startsAtUtc) <= 30 * 60;
        }
        if (!showNext) {
            return {
                floor,
                previous->item,
                previous->startsAtUtc,
                previous->endsAtUtc,
                FloorScheduleStatus::Ended,
            };
        }
    }

    if (
        next.has_value()
        && nowUtc.secsTo(next->startsAtUtc) <= 30 * 60
    ) {
        return {
            floor,
            next->item,
            next->startsAtUtc,
            next->endsAtUtc,
            upcomingStatus(next->startsAtUtc, nowUtc),
        };
    }

    return {floor, std::nullopt, {}, {}, FloorScheduleStatus::Empty};
}

QString statusCode(const FloorScheduleStatus status)
{
    switch (status) {
    case FloorScheduleStatus::Inactive:
        return QStringLiteral("inactive");
    case FloorScheduleStatus::Upcoming:
        return QStringLiteral("upcoming");
    case FloorScheduleStatus::StartingSoon:
        return QStringLiteral("starting_soon");
    case FloorScheduleStatus::Active:
        return QStringLiteral("active");
    case FloorScheduleStatus::Ended:
        return QStringLiteral("ended");
    case FloorScheduleStatus::Empty:
    default:
        return QStringLiteral("empty");
    }
}

QString statusLabel(const FloorScheduleStatus status)
{
    switch (status) {
    case FloorScheduleStatus::Inactive:
        return QStringLiteral("오늘 일정");
    case FloorScheduleStatus::Upcoming:
        return QStringLiteral("30분 전");
    case FloorScheduleStatus::StartingSoon:
        return QStringLiteral("곧 시작");
    case FloorScheduleStatus::Active:
        return QStringLiteral("ON AIR");
    case FloorScheduleStatus::Ended:
        return QStringLiteral("종료");
    case FloorScheduleStatus::Empty:
    default:
        return {};
    }
}

}  // namespace

QVector<ScheduleRow> ScheduleProjection::at(
    const QVector<ScheduleItem> &items,
    const QDateTime &nowUtc,
    const int limit
)
{
    if (!nowUtc.isValid() || limit <= 0) {
        return {};
    }

    const QTimeZone seoul(QByteArrayLiteral("Asia/Seoul"));
    const QDate today = nowUtc.toTimeZone(seoul).date();

    QVector<ScheduleRow> active;
    QVector<ScheduleRow> upcoming;
    for (const ScheduleItem &item : items) {
        if (item.date.has_value()) {
            if (*item.date != today && *item.date != today.addDays(-1)) {
                continue;
            }
            const auto occurrence = occurrenceOn(item, *item.date, nowUtc);
            if (!occurrence.has_value()) {
                continue;
            }
            if (
                *item.date == today.addDays(-1)
                && occurrence->status != ScheduleStatus::Active
            ) {
                continue;
            }
            if (occurrence->status == ScheduleStatus::Active) {
                active.push_back(*occurrence);
            } else {
                upcoming.push_back(*occurrence);
            }
            continue;
        }

        for (int dayOffset = -1; dayOffset <= 0; ++dayOffset) {
            const auto occurrence = occurrenceOn(
                item,
                today.addDays(dayOffset),
                nowUtc
            );
            if (!occurrence.has_value()) {
                continue;
            }
            if (
                dayOffset == -1
                && occurrence->status != ScheduleStatus::Active
            ) {
                continue;
            }
            if (occurrence->status == ScheduleStatus::Active) {
                active.push_back(*occurrence);
            } else {
                upcoming.push_back(*occurrence);
            }
        }
    }

    const auto priorityThenStart = [](
        const ScheduleRow &left,
        const ScheduleRow &right
    ) {
        if (left.item.priority != right.item.priority) {
            return left.item.priority > right.item.priority;
        }
        if (left.startsAtUtc != right.startsAtUtc) {
            return left.startsAtUtc < right.startsAtUtc;
        }
        return left.item.id < right.item.id;
    };
    std::sort(active.begin(), active.end(), priorityThenStart);
    std::sort(upcoming.begin(), upcoming.end(), priorityThenStart);

    QVector<ScheduleRow> result;
    result.reserve(std::min<qsizetype>(
        limit,
        active.size() + upcoming.size()
    ));
    for (const ScheduleRow &row : active) {
        if (result.size() == limit) {
            return result;
        }
        result.push_back(row);
    }
    for (const ScheduleRow &row : upcoming) {
        if (result.size() == limit) {
            break;
        }
        result.push_back(row);
    }
    return result;
}

QVector<FloorScheduleRow> ScheduleProjection::forFloors(
    const QVector<ScheduleItem> &items,
    const QDateTime &nowUtc
)
{
    QVector<FloorScheduleRow> result;
    if (!nowUtc.isValid()) {
        return result;
    }

    const QTimeZone seoul(QByteArrayLiteral("Asia/Seoul"));
    const QDate today = nowUtc.toTimeZone(seoul).date();
    for (const int floor : {4, 3, 2}) {
        QVector<Occurrence> occurrences;
        for (const ScheduleItem &item : items) {
            if (floorNumber(item.location) != floor) {
                continue;
            }
            if (item.date.has_value()) {
                if (
                    *item.date != today
                    && *item.date != today.addDays(-1)
                    && *item.date != today.addDays(1)
                ) {
                    continue;
                }
                const auto occurrence =
                    occurrenceIncludingEnded(item, *item.date);
                if (occurrence.has_value()) {
                    occurrences.push_back(*occurrence);
                }
                continue;
            }
            for (int offset = -1; offset <= 1; ++offset) {
                const auto occurrence =
                    occurrenceIncludingEnded(item, today.addDays(offset));
                if (occurrence.has_value()) {
                    occurrences.push_back(*occurrence);
                }
            }
        }
        result.push_back(selectFloorRow(floor, occurrences, nowUtc));
    }
    return result;
}

ScheduleListModel::ScheduleListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ScheduleListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant ScheduleListModel::data(const QModelIndex &index, const int role) const
{
    if (
        !index.isValid() || index.row() < 0
        || index.row() >= m_rows.size()
    ) {
        return {};
    }

    const ScheduleRow &row = m_rows[index.row()];
    const QTimeZone seoul(QByteArrayLiteral("Asia/Seoul"));
    switch (role) {
    case IdRole:
        return row.item.id;
    case TitleRole:
        return row.item.title;
    case LocationRole:
        return row.item.location;
    case StatusRole:
        return row.status == ScheduleStatus::Active
            ? QStringLiteral("진행 중")
            : QStringLiteral("예정");
    case TimeRole: {
        const QDateTime start = row.startsAtUtc.toTimeZone(seoul);
        const QDateTime end = row.endsAtUtc.toTimeZone(seoul);
        return QStringLiteral("%1–%2")
            .arg(start.toString(QStringLiteral("HH:mm")))
            .arg(end.toString(QStringLiteral("HH:mm")));
    }
    case DayRole: {
        const QDate localDate = row.startsAtUtc.toTimeZone(seoul).date();
        const QLocale korean(QLocale::Korean, QLocale::SouthKorea);
        if (row.item.date.has_value()) {
            return korean.toString(localDate, QStringLiteral("M월 d일(ddd)"));
        }
        return QStringLiteral("매주 %1")
            .arg(korean.dayName(localDate.dayOfWeek(), QLocale::LongFormat));
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> ScheduleListModel::roleNames() const
{
    return {
        {IdRole, "eventId"},
        {TitleRole, "title"},
        {LocationRole, "location"},
        {StatusRole, "status"},
        {TimeRole, "timeRange"},
        {DayRole, "dayLabel"},
    };
}

void ScheduleListModel::setItems(const QVector<ScheduleItem> &items)
{
    m_items = items;
    refresh(QDateTime::currentDateTimeUtc());
}

void ScheduleListModel::refresh(const QDateTime &nowUtc)
{
    const QVector<ScheduleRow> rows = ScheduleProjection::at(
        m_items,
        nowUtc
    );
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

FloorScheduleModel::FloorScheduleModel(QObject *parent)
    : QAbstractListModel(parent)
{
    refresh(QDateTime::currentDateTimeUtc());
}

int FloorScheduleModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant FloorScheduleModel::data(
    const QModelIndex &index,
    const int role
) const
{
    if (
        !index.isValid() || index.row() < 0
        || index.row() >= m_rows.size()
    ) {
        return {};
    }

    const FloorScheduleRow &row = m_rows[index.row()];
    switch (role) {
    case FloorRole:
        return row.floor;
    case IdRole:
        return row.item.has_value() ? row.item->id : QString();
    case TitleRole:
        return row.item.has_value() ? row.item->title : QString();
    case LocationRole:
        return row.item.has_value() ? row.item->location : QString();
    case StatusCodeRole:
        return statusCode(row.status);
    case StatusLabelRole:
        return statusLabel(row.status);
    case TimeRole:
        if (!row.item.has_value()) {
            return QString();
        }
        {
            const int hour24 = row.item->startTime.hour();
            int hour12 = hour24 % 12;
            if (hour12 == 0) {
                hour12 = 12;
            }
            const QString period = hour24 < 12
                ? QStringLiteral("오전")
                : QStringLiteral("오후");
            return row.item->startTime.minute() == 0
                ? QStringLiteral("%1 %2시").arg(period).arg(hour12)
                : QStringLiteral("%1 %2:%3")
                    .arg(period)
                    .arg(hour12)
                    .arg(
                        row.item->startTime.minute(),
                        2,
                        10,
                        QLatin1Char('0')
                    );
        }
    case HasEventRole:
        return row.item.has_value();
    default:
        return {};
    }
}

QHash<int, QByteArray> FloorScheduleModel::roleNames() const
{
    return {
        {FloorRole, "floor"},
        {IdRole, "eventId"},
        {TitleRole, "title"},
        {LocationRole, "location"},
        {StatusCodeRole, "statusCode"},
        {StatusLabelRole, "statusLabel"},
        {TimeRole, "timeRange"},
        {HasEventRole, "hasEvent"},
    };
}

void FloorScheduleModel::setItems(const QVector<ScheduleItem> &items)
{
    m_items = items;
    refresh(QDateTime::currentDateTimeUtc());
}

void FloorScheduleModel::refresh(const QDateTime &nowUtc)
{
    beginResetModel();
    m_rows = ScheduleProjection::forFloors(m_items, nowUtc);
    endResetModel();
}

}  // namespace signage::display
