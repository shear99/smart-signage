#pragma once

#include <QDate>
#include <QString>
#include <QTime>
#include <QVector>

#include <optional>

namespace signage::display {

enum class DisplayMode {
    Dashboard,
    FullscreenMedia,
};

enum class MediaFit {
    Fit,
    Crop,
};

enum class ActivationPolicy {
    Immediate,
    AfterCurrentItem,
};

struct EdgeInsets {
    qreal top = 0.0;
    qreal right = 0.0;
    qreal bottom = 0.0;
    qreal left = 0.0;

    friend bool operator==(const EdgeInsets &, const EdgeInsets &) = default;
};

struct MediaItem {
    QString id;
    int order = 0;
    QString absolutePath;
    std::optional<MediaFit> fit;
    std::optional<bool> muted;
    std::optional<qreal> volume;

    friend bool operator==(const MediaItem &, const MediaItem &) = default;
};

struct ScheduleItem {
    QString id;
    QString title;
    QString location;
    QVector<int> daysOfWeek;
    std::optional<QDate> date;
    QTime startTime;
    QTime endTime;
    bool endsNextDay = false;
    int priority = 0;

    friend bool operator==(const ScheduleItem &, const ScheduleItem &) = default;
};

struct SubtitleMessage {
    QString id;
    QString text;
    QString richText;
    int durationMs = 5'000;
    QString effect = QStringLiteral("fade");
    int fontSizePx = 60;
    int transitionMs = 700;

    friend bool operator==(const SubtitleMessage &, const SubtitleMessage &) = default;
};

struct BundleSnapshot {
    QString bundleId;
    QString releaseRoot;
    DisplayMode displayMode = DisplayMode::Dashboard;
    MediaFit defaultMediaFit = MediaFit::Fit;
    bool defaultMuted = false;
    qreal defaultVolume = 1.0;
    ActivationPolicy playlistActivation =
        ActivationPolicy::AfterCurrentItem;
    EdgeInsets safeInsets;
    QVector<MediaItem> playlist;
    QVector<ScheduleItem> schedule;
    QVector<SubtitleMessage> subtitles;
};

}  // namespace signage::display
