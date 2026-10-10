#pragma once

#include <QVector>

#include <optional>

namespace signage::display {

class PlaylistNavigator {
public:
    void reset(int itemCount);

    [[nodiscard]] std::optional<int> begin(qint64 nowMs);
    [[nodiscard]] std::optional<int> advance(qint64 nowMs);
    void reportFailure(qint64 nowMs);
    void reportSuccess();

    [[nodiscard]] int currentIndex() const;
    [[nodiscard]] int itemCount() const;
    [[nodiscard]] qint64 millisecondsUntilNextRetry(qint64 nowMs) const;

private:
    struct FailureState {
        int consecutiveFailures = 0;
        qint64 retryAfterMs = 0;
    };

    [[nodiscard]] std::optional<int> findAvailable(
        int startIndex,
        qint64 nowMs
    ) const;

    QVector<FailureState> m_failures;
    int m_currentIndex = -1;
};

}  // namespace signage::display
