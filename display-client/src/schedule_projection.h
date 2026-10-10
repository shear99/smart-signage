#pragma once

#include "bundle_types.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QVector>

namespace signage::display {

enum class ScheduleStatus {
    Active,
    Upcoming,
};

enum class FloorScheduleStatus {
    Empty,
    Inactive,
    Upcoming,
    StartingSoon,
    Active,
    Ended,
};

struct ScheduleRow {
    ScheduleItem item;
    QDateTime startsAtUtc;
    QDateTime endsAtUtc;
    ScheduleStatus status = ScheduleStatus::Upcoming;
};

struct FloorScheduleRow {
    int floor = 0;
    std::optional<ScheduleItem> item;
    QDateTime startsAtUtc;
    QDateTime endsAtUtc;
    FloorScheduleStatus status = FloorScheduleStatus::Empty;
};

class ScheduleProjection {
public:
    [[nodiscard]] static QVector<ScheduleRow> at(
        const QVector<ScheduleItem> &items,
        const QDateTime &nowUtc,
        int limit = 8
    );

    [[nodiscard]] static QVector<FloorScheduleRow> forFloors(
        const QVector<ScheduleItem> &items,
        const QDateTime &nowUtc
    );
};

class ScheduleListModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        LocationRole,
        StatusRole,
        TimeRole,
        DayRole,
    };
    Q_ENUM(Role)

    explicit ScheduleListModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex &parent = QModelIndex()
    ) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex &index,
        int role
    ) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setItems(const QVector<ScheduleItem> &items);
    void refresh(const QDateTime &nowUtc);

private:
    QVector<ScheduleItem> m_items;
    QVector<ScheduleRow> m_rows;
};

class FloorScheduleModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        FloorRole = Qt::UserRole + 1,
        IdRole,
        TitleRole,
        LocationRole,
        StatusCodeRole,
        StatusLabelRole,
        TimeRole,
        HasEventRole,
    };
    Q_ENUM(Role)

    explicit FloorScheduleModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex &parent = QModelIndex()
    ) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex &index,
        int role
    ) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setItems(const QVector<ScheduleItem> &items);
    void refresh(const QDateTime &nowUtc);

private:
    QVector<ScheduleItem> m_items;
    QVector<FloorScheduleRow> m_rows;
};

}  // namespace signage::display
