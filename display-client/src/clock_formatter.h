#pragma once

#include <QDateTime>
#include <QString>

namespace signage::display {

struct ClockParts {
    QString period;
    QString time;
    QString date;

    friend bool operator==(const ClockParts &, const ClockParts &) = default;
};

class ClockFormatter {
public:
    [[nodiscard]] static ClockParts korean12Hour(const QDateTime &nowUtc);
};

}  // namespace signage::display
