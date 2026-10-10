#include "clock_formatter.h"

#include <QLocale>
#include <QTimeZone>

namespace signage::display {

ClockParts ClockFormatter::korean12Hour(const QDateTime &nowUtc)
{
    if (!nowUtc.isValid()) {
        return {};
    }

    const QTimeZone seoul(QByteArrayLiteral("Asia/Seoul"));
    const QLocale korean(QLocale::Korean, QLocale::SouthKorea);
    const QDateTime local = nowUtc.toTimeZone(seoul);
    const int hour24 = local.time().hour();
    int hour12 = hour24 % 12;
    if (hour12 == 0) {
        hour12 = 12;
    }

    return {
        hour24 < 12 ? QStringLiteral("오전") : QStringLiteral("오후"),
        QStringLiteral("%1:%2:%3")
            .arg(hour12, 2, 10, QLatin1Char('0'))
            .arg(local.time().minute(), 2, 10, QLatin1Char('0'))
            .arg(local.time().second(), 2, 10, QLatin1Char('0')),
        korean.toString(local.date(), QStringLiteral("M월 d일 dddd")),
    };
}

}  // namespace signage::display
