#pragma once

#include "active_bundle_watcher.h"
#include "bundle_types.h"
#include "clock_formatter.h"
#include "playlist_navigator.h"
#include "schedule_projection.h"

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <optional>

namespace signage::display {

class DisplayController final : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString bundleId READ bundleId NOTIFY bundleChanged)
    Q_PROPERTY(QString displayMode READ displayMode NOTIFY bundleChanged)
    Q_PROPERTY(bool cropMedia READ cropMedia NOTIFY mediaSettingsChanged)
    Q_PROPERTY(qreal safeTop READ safeTop NOTIFY bundleChanged)
    Q_PROPERTY(qreal safeRight READ safeRight NOTIFY bundleChanged)
    Q_PROPERTY(qreal safeBottom READ safeBottom NOTIFY bundleChanged)
    Q_PROPERTY(qreal safeLeft READ safeLeft NOTIFY bundleChanged)

    Q_PROPERTY(
        QUrl currentMediaSource
        READ currentMediaSource
        NOTIFY playbackChanged
    )
    Q_PROPERTY(
        QString currentMediaTitle
        READ currentMediaTitle
        NOTIFY playbackChanged
    )
    Q_PROPERTY(
        bool currentMediaMuted
        READ currentMediaMuted
        NOTIFY mediaSettingsChanged
    )
    Q_PROPERTY(
        qreal currentMediaVolume
        READ currentMediaVolume
        NOTIFY mediaSettingsChanged
    )
    Q_PROPERTY(
        quint64 playbackSerial
        READ playbackSerial
        NOTIFY playbackChanged
    )

    Q_PROPERTY(QString subtitleText READ subtitleText NOTIFY subtitleChanged)
    Q_PROPERTY(
        QString subtitleRichText
        READ subtitleRichText
        NOTIFY subtitleChanged
    )
    Q_PROPERTY(
        QString subtitleEffect
        READ subtitleEffect
        NOTIFY subtitleChanged
    )
    Q_PROPERTY(
        int subtitleFontSize
        READ subtitleFontSize
        NOTIFY subtitleChanged
    )
    Q_PROPERTY(
        int subtitleTransitionMs
        READ subtitleTransitionMs
        NOTIFY subtitleChanged
    )
    Q_PROPERTY(QString nowText READ nowText NOTIFY nowTextChanged)
    Q_PROPERTY(
        QString clockPeriodText
        READ clockPeriodText
        NOTIFY nowTextChanged
    )
    Q_PROPERTY(
        QString clockTimeText
        READ clockTimeText
        NOTIFY nowTextChanged
    )
    Q_PROPERTY(
        QString clockDateText
        READ clockDateText
        NOTIFY nowTextChanged
    )
    Q_PROPERTY(
        QString fallbackMessage
        READ fallbackMessage
        NOTIFY fallbackChanged
    )
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(
        QAbstractItemModel *scheduleModel
        READ scheduleModel
        CONSTANT
    )

public:
    explicit DisplayController(
        const QString &contentRoot,
        QObject *parent = nullptr
    );

    [[nodiscard]] QString bundleId() const;
    [[nodiscard]] QString displayMode() const;
    [[nodiscard]] bool cropMedia() const;
    [[nodiscard]] qreal safeTop() const;
    [[nodiscard]] qreal safeRight() const;
    [[nodiscard]] qreal safeBottom() const;
    [[nodiscard]] qreal safeLeft() const;

    [[nodiscard]] QUrl currentMediaSource() const;
    [[nodiscard]] QString currentMediaTitle() const;
    [[nodiscard]] bool currentMediaMuted() const;
    [[nodiscard]] qreal currentMediaVolume() const;
    [[nodiscard]] quint64 playbackSerial() const;

    [[nodiscard]] QString subtitleText() const;
    [[nodiscard]] QString subtitleRichText() const;
    [[nodiscard]] QString subtitleEffect() const;
    [[nodiscard]] int subtitleFontSize() const;
    [[nodiscard]] int subtitleTransitionMs() const;
    [[nodiscard]] QString nowText() const;
    [[nodiscard]] QString clockPeriodText() const;
    [[nodiscard]] QString clockTimeText() const;
    [[nodiscard]] QString clockDateText() const;
    [[nodiscard]] QString fallbackMessage() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QAbstractItemModel *scheduleModel();

public slots:
    void mediaEnded();
    void mediaFailed(const QString &message);
    void mediaPlaying();

signals:
    void bundleChanged();
    void playbackChanged();
    void mediaSettingsChanged();
    void subtitleChanged();
    void nowTextChanged();
    void fallbackChanged();
    void statusChanged();

private:
    void acceptSnapshot(const BundleSnapshot &snapshot);
    void applySettings(const BundleSnapshot &snapshot);
    void applyPlaylist(const BundleSnapshot &snapshot);
    void selectMedia(std::optional<int> index);
    void showNoPlayableMedia();
    void advanceSubtitle();
    void updateClock();
    void setFallbackMessage(const QString &message);
    void setStatusText(const QString &message);
    [[nodiscard]] QString currentItemId() const;

    ActiveBundleWatcher m_watcher;
    FloorScheduleModel m_scheduleModel;
    PlaylistNavigator m_playlistNavigator;
    QVector<MediaItem> m_playlist;
    QVector<SubtitleMessage> m_subtitles;
    std::optional<BundleSnapshot> m_activeSnapshot;
    std::optional<BundleSnapshot> m_pendingSnapshot;

    QTimer m_retryTimer;
    QTimer m_subtitleTimer;
    QTimer m_clockTimer;
    QTimer m_scheduleTimer;

    QUrl m_currentMediaSource;
    QString m_currentMediaTitle;
    QString m_playlistBundleId;
    quint64 m_playbackSerial = 0;
    bool m_currentItemActive = false;

    int m_subtitleIndex = -1;
    QString m_subtitleText;
    QString m_subtitleRichText;
    QString m_subtitleEffect = QStringLiteral("fade");
    int m_subtitleFontSize = 60;
    int m_subtitleTransitionMs = 700;
    QString m_nowText;
    QString m_clockPeriodText;
    QString m_clockTimeText;
    QString m_clockDateText;
    QString m_fallbackMessage =
        QStringLiteral("콘텐츠를 준비하고 있습니다");
    QString m_statusText;
};

}  // namespace signage::display
