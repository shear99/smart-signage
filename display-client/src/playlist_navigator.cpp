#include "playlist_navigator.h"

#include <algorithm>
#include <limits>

namespace signage::display {

void PlaylistNavigator::reset(const int itemCount)
{
    m_failures = QVector<FailureState>(std::max(0, itemCount));
    m_currentIndex = -1;
}

std::optional<int> PlaylistNavigator::begin(const qint64 nowMs)
{
    const auto available = findAvailable(0, nowMs);
    m_currentIndex = available.value_or(-1);
    return available;
}

std::optional<int> PlaylistNavigator::advance(const qint64 nowMs)
{
    if (m_failures.isEmpty()) {
        m_currentIndex = -1;
        return std::nullopt;
    }

    const int start =
        m_currentIndex < 0 ? 0 : (m_currentIndex + 1) % m_failures.size();
    const auto available = findAvailable(start, nowMs);
    m_currentIndex = available.value_or(-1);
    return available;
}

void PlaylistNavigator::reportFailure(const qint64 nowMs)
{
    if (m_currentIndex < 0 || m_currentIndex >= m_failures.size()) {
        return;
    }

    FailureState &state = m_failures[m_currentIndex];
    state.consecutiveFailures =
        std::min(state.consecutiveFailures + 1, 30);
    const int exponent = std::min(state.consecutiveFailures - 1, 5);
    const qint64 delayMs = std::min<qint64>(
        30'000,
        1'000LL << exponent
    );
    state.retryAfterMs = nowMs + delayMs;
}

void PlaylistNavigator::reportSuccess()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_failures.size()) {
        return;
    }
    m_failures[m_currentIndex] = {};
}

int PlaylistNavigator::currentIndex() const
{
    return m_currentIndex;
}

int PlaylistNavigator::itemCount() const
{
    return m_failures.size();
}

qint64 PlaylistNavigator::millisecondsUntilNextRetry(const qint64 nowMs) const
{
    qint64 nextRetry = std::numeric_limits<qint64>::max();
    for (const FailureState &state : m_failures) {
        if (state.retryAfterMs > nowMs) {
            nextRetry = std::min(nextRetry, state.retryAfterMs);
        }
    }
    if (nextRetry == std::numeric_limits<qint64>::max()) {
        return 0;
    }
    return std::max<qint64>(1, nextRetry - nowMs);
}

std::optional<int> PlaylistNavigator::findAvailable(
    const int startIndex,
    const qint64 nowMs
) const
{
    if (m_failures.isEmpty()) {
        return std::nullopt;
    }

    for (int offset = 0; offset < m_failures.size(); ++offset) {
        const int index = (startIndex + offset) % m_failures.size();
        if (m_failures[index].retryAfterMs <= nowMs) {
            return index;
        }
    }
    return std::nullopt;
}

}  // namespace signage::display
