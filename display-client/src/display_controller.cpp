#include "display_controller.h"

#include <QDateTime>
#include <QDebug>
#include <QLocale>
#include <QTimeZone>

#include <algorithm>

namespace signage::display {

DisplayController::DisplayController(
    const QString &contentRoot,
    QObject *parent
)
    : QObject(parent)
    , m_watcher(contentRoot, this)
    , m_scheduleModel(this)
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, [this]() {
        qInfo().noquote()
            << "Display playback retry started"
            << "playlist_bundle=" << m_playlistBundleId;
        selectMedia(
            m_playlistNavigator.begin(QDateTime::currentMSecsSinceEpoch())
        );
    });

    connect(
        &m_subtitleTimer,
        &QTimer::timeout,
        this,
        &DisplayController::advanceSubtitle
    );
    m_subtitleTimer.setSingleShot(true);

    m_clockTimer.setInterval(1'000);
    connect(
        &m_clockTimer,
        &QTimer::timeout,
        this,
        &DisplayController::updateClock
    );
    m_clockTimer.start();
    updateClock();

    m_scheduleTimer.setInterval(10'000);
    connect(&m_scheduleTimer, &QTimer::timeout, this, [this]() {
        m_scheduleModel.refresh(QDateTime::currentDateTimeUtc());
    });
    m_scheduleTimer.start();

    connect(
        &m_watcher,
        &ActiveBundleWatcher::snapshotReady,
        this,
        &DisplayController::acceptSnapshot
    );
    connect(
        &m_watcher,
        &ActiveBundleWatcher::loadFailed,
        this,
        [this](const QString &error) {
            qWarning().noquote()
                << "Active display bundle rejected; keeping last-known-good:"
                << error;
            setStatusText(
                m_activeSnapshot.has_value()
                    ? QStringLiteral("새 콘텐츠 검증 실패 · 이전 화면 유지")
                    : QStringLiteral("유효한 콘텐츠를 기다리는 중")
            );
            if (!m_activeSnapshot.has_value()) {
                setFallbackMessage(QStringLiteral("콘텐츠를 준비하고 있습니다"));
            }
        }
    );
}

QString DisplayController::bundleId() const
{
    return m_activeSnapshot.has_value()
        ? m_activeSnapshot->bundleId
        : QString();
}

QString DisplayController::displayMode() const
{
    if (
        m_activeSnapshot.has_value()
        && m_activeSnapshot->displayMode == DisplayMode::FullscreenMedia
    ) {
        return QStringLiteral("fullscreen_media");
    }
    return QStringLiteral("dashboard");
}

bool DisplayController::cropMedia() const
{
    MediaFit fit = m_activeSnapshot.has_value()
        ? m_activeSnapshot->defaultMediaFit
        : MediaFit::Fit;
    const int current = m_playlistNavigator.currentIndex();
    if (
        current >= 0 && current < m_playlist.size()
        && m_playlist[current].fit.has_value()
    ) {
        fit = *m_playlist[current].fit;
    }
    return fit == MediaFit::Crop;
}

qreal DisplayController::safeTop() const
{
    return m_activeSnapshot.has_value()
        ? m_activeSnapshot->safeInsets.top
        : 0.0;
}

qreal DisplayController::safeRight() const
{
    return m_activeSnapshot.has_value()
        ? m_activeSnapshot->safeInsets.right
        : 0.0;
}

qreal DisplayController::safeBottom() const
{
    return m_activeSnapshot.has_value()
        ? m_activeSnapshot->safeInsets.bottom
        : 0.0;
}

qreal DisplayController::safeLeft() const
{
    return m_activeSnapshot.has_value()
        ? m_activeSnapshot->safeInsets.left
        : 0.0;
}

QUrl DisplayController::currentMediaSource() const
{
    return m_currentMediaSource;
}

QString DisplayController::currentMediaTitle() const
{
    return m_currentMediaTitle;
}

bool DisplayController::currentMediaMuted() const
{
    const bool globallyMuted = m_activeSnapshot.has_value()
        ? m_activeSnapshot->defaultMuted
        : true;
    if (globallyMuted) {
        return true;
    }
    const int current = m_playlistNavigator.currentIndex();
    if (
        current >= 0 && current < m_playlist.size()
        && m_playlist[current].muted.has_value()
    ) {
        return *m_playlist[current].muted;
    }
    return false;
}

qreal DisplayController::currentMediaVolume() const
{
    qreal volume = m_activeSnapshot.has_value()
        ? m_activeSnapshot->defaultVolume
        : 0.0;
    const int current = m_playlistNavigator.currentIndex();
    if (
        current >= 0 && current < m_playlist.size()
        && m_playlist[current].volume.has_value()
    ) {
        volume = *m_playlist[current].volume;
    }
    return std::clamp<qreal>(volume, 0.0, 1.0);
}

quint64 DisplayController::playbackSerial() const
{
    return m_playbackSerial;
}

QString DisplayController::subtitleText() const
{
    return m_subtitleText;
}

QString DisplayController::subtitleRichText() const
{
    return m_subtitleRichText;
}

QString DisplayController::subtitleEffect() const
{
    return m_subtitleEffect;
}

int DisplayController::subtitleFontSize() const
{
    return m_subtitleFontSize;
}

int DisplayController::subtitleTransitionMs() const
{
    return m_subtitleTransitionMs;
}

QString DisplayController::nowText() const
{
    return m_nowText;
}

QString DisplayController::clockPeriodText() const
{
    return m_clockPeriodText;
}

QString DisplayController::clockTimeText() const
{
    return m_clockTimeText;
}

QString DisplayController::clockDateText() const
{
    return m_clockDateText;
}

QString DisplayController::fallbackMessage() const
{
    return m_fallbackMessage;
}

QString DisplayController::statusText() const
{
    return m_statusText;
}

QAbstractItemModel *DisplayController::scheduleModel()
{
    return &m_scheduleModel;
}

void DisplayController::mediaEnded()
{
    if (!m_currentItemActive) {
        return;
    }
    qInfo().noquote()
        << "Display playback ended"
        << "playlist_bundle=" << m_playlistBundleId
        << "item=" << currentItemId()
        << "serial=" << m_playbackSerial;
    m_currentItemActive = false;
    m_playlistNavigator.reportSuccess();

    if (m_pendingSnapshot.has_value()) {
        const BundleSnapshot pending = *m_pendingSnapshot;
        m_pendingSnapshot.reset();
        applyPlaylist(pending);
        return;
    }

    selectMedia(
        m_playlistNavigator.advance(QDateTime::currentMSecsSinceEpoch())
    );
}

void DisplayController::mediaFailed(const QString &message)
{
    if (!m_currentItemActive) {
        return;
    }
    const QString failedItemId = currentItemId();
    m_currentItemActive = false;
    qWarning().noquote()
        << "Display playback error"
        << "playlist_bundle=" << m_playlistBundleId
        << "item=" << failedItemId
        << "serial=" << m_playbackSerial
        << "detail_available=" << !message.isEmpty();
    setFallbackMessage(QStringLiteral("영상을 재생할 수 없습니다"));

    if (m_pendingSnapshot.has_value()) {
        const BundleSnapshot pending = *m_pendingSnapshot;
        m_pendingSnapshot.reset();
        qInfo().noquote()
            << "Display pending playlist activated after playback error"
            << "bundle=" << pending.bundleId;
        applyPlaylist(pending);
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_playlistNavigator.reportFailure(nowMs);
    const std::optional<int> next = m_playlistNavigator.advance(nowMs);
    if (next.has_value()) {
        selectMedia(next);
        return;
    }
    showNoPlayableMedia();
}

void DisplayController::mediaPlaying()
{
    if (!m_currentItemActive) {
        return;
    }
    qInfo().noquote()
        << "Display playback entered PlayingState"
        << "playlist_bundle=" << m_playlistBundleId
        << "item=" << currentItemId()
        << "serial=" << m_playbackSerial;
    setFallbackMessage({});
    setStatusText({});
}

void DisplayController::acceptSnapshot(const BundleSnapshot &snapshot)
{
    applySettings(snapshot);
    if (
        snapshot.playlistActivation == ActivationPolicy::AfterCurrentItem
        && m_currentItemActive
    ) {
        m_pendingSnapshot = snapshot;
        qInfo().noquote()
            << "Display playlist deferred until playback boundary"
            << "bundle=" << snapshot.bundleId;
        setStatusText(QStringLiteral("현재 항목 종료 후 새 콘텐츠 적용"));
        return;
    }
    m_pendingSnapshot.reset();
    applyPlaylist(snapshot);
}

void DisplayController::applySettings(const BundleSnapshot &snapshot)
{
    m_activeSnapshot = snapshot;
    m_scheduleModel.setItems(snapshot.schedule);

    m_subtitleTimer.stop();
    m_subtitles = snapshot.subtitles;
    m_subtitleIndex = -1;
    if (m_subtitles.isEmpty()) {
        if (!m_subtitleText.isEmpty() || !m_subtitleRichText.isEmpty()) {
            m_subtitleText.clear();
            m_subtitleRichText.clear();
            m_subtitleEffect = QStringLiteral("fade");
            m_subtitleFontSize = 60;
            m_subtitleTransitionMs = 700;
            emit subtitleChanged();
        }
    } else {
        advanceSubtitle();
    }

    setStatusText({});
    emit bundleChanged();
    emit mediaSettingsChanged();
}

void DisplayController::applyPlaylist(const BundleSnapshot &snapshot)
{
    m_retryTimer.stop();
    m_playlist = snapshot.playlist;
    m_playlistBundleId = snapshot.bundleId;
    m_playlistNavigator.reset(m_playlist.size());
    qInfo().noquote()
        << "Display playlist activated"
        << "bundle=" << snapshot.bundleId
        << "items=" << m_playlist.size();
    selectMedia(
        m_playlistNavigator.begin(QDateTime::currentMSecsSinceEpoch())
    );
}

void DisplayController::selectMedia(const std::optional<int> index)
{
    if (
        !index.has_value() || *index < 0 || *index >= m_playlist.size()
    ) {
        showNoPlayableMedia();
        return;
    }

    const MediaItem &item = m_playlist[*index];
    m_currentMediaSource = QUrl::fromLocalFile(item.absolutePath);
    m_currentMediaTitle.clear();
    m_currentItemActive = true;
    ++m_playbackSerial;
    setFallbackMessage(QStringLiteral("영상을 불러오는 중"));
    qInfo().noquote()
        << "Display playback start requested"
        << "playlist_bundle=" << m_playlistBundleId
        << "item=" << item.id
        << "index=" << *index
        << "serial=" << m_playbackSerial;
    emit playbackChanged();
    emit mediaSettingsChanged();
}

void DisplayController::showNoPlayableMedia()
{
    m_currentItemActive = false;
    m_currentMediaSource = QUrl();
    m_currentMediaTitle.clear();
    ++m_playbackSerial;
    emit playbackChanged();
    emit mediaSettingsChanged();
    setFallbackMessage(
        m_playlist.isEmpty()
            ? QStringLiteral("재생할 콘텐츠가 없습니다")
            : QStringLiteral("영상을 재생할 수 없습니다 · 잠시 후 다시 시도합니다")
    );

    if (!m_playlist.isEmpty()) {
        const qint64 delay = m_playlistNavigator.millisecondsUntilNextRetry(
            QDateTime::currentMSecsSinceEpoch()
        );
        m_retryTimer.start(
            static_cast<int>(std::clamp<qint64>(delay, 1, 30'000))
        );
        qWarning().noquote()
            << "Display playback retry scheduled"
            << "playlist_bundle=" << m_playlistBundleId
            << "delay_ms=" << m_retryTimer.interval();
    }
}

void DisplayController::advanceSubtitle()
{
    if (m_subtitles.isEmpty()) {
        return;
    }
    m_subtitleIndex = (m_subtitleIndex + 1) % m_subtitles.size();
    m_subtitleText = m_subtitles[m_subtitleIndex].text;
    m_subtitleRichText = m_subtitles[m_subtitleIndex].richText;
    m_subtitleEffect = m_subtitles[m_subtitleIndex].effect;
    m_subtitleFontSize = m_subtitles[m_subtitleIndex].fontSizePx;
    m_subtitleTransitionMs = m_subtitles[m_subtitleIndex].transitionMs;
    emit subtitleChanged();
    if (m_subtitles.size() > 1) {
        m_subtitleTimer.start(m_subtitles[m_subtitleIndex].durationMs);
    }
}

void DisplayController::updateClock()
{
    const ClockParts parts = ClockFormatter::korean12Hour(
        QDateTime::currentDateTimeUtc()
    );
    const QString value = QStringLiteral("%1 %2 · %3")
        .arg(parts.period, parts.time, parts.date);
    if (
        value != m_nowText
        || parts.period != m_clockPeriodText
        || parts.time != m_clockTimeText
        || parts.date != m_clockDateText
    ) {
        m_nowText = value;
        m_clockPeriodText = parts.period;
        m_clockTimeText = parts.time;
        m_clockDateText = parts.date;
        emit nowTextChanged();
    }
}

void DisplayController::setFallbackMessage(const QString &message)
{
    if (message == m_fallbackMessage) {
        return;
    }
    m_fallbackMessage = message;
    emit fallbackChanged();
}

void DisplayController::setStatusText(const QString &message)
{
    if (message == m_statusText) {
        return;
    }
    m_statusText = message;
    emit statusChanged();
}

QString DisplayController::currentItemId() const
{
    const int current = m_playlistNavigator.currentIndex();
    if (current < 0 || current >= m_playlist.size()) {
        return QStringLiteral("none");
    }
    return m_playlist[current].id;
}

}  // namespace signage::display
