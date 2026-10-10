#include "active_bundle_watcher.h"
#include "bundle_parser.h"
#include "clock_formatter.h"
#include "display_controller.h"
#include "layout_geometry.h"
#include "playlist_navigator.h"
#include "schedule_projection.h"
#include "screen_selector.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <optional>

using namespace signage::display;

namespace {

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size()
        && file.flush();
}

QByteArray manifestBytes(const int revision)
{
    return QStringLiteral(
        R"JSON({"files":[],"revision":%1,"schema_version":1})JSON"
    ).arg(revision).toUtf8();
}

QString manifestSha(const int revision)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            manifestBytes(revision),
            QCryptographicHash::Sha256
        ).toHex()
    );
}

QString bundleId(const int revision)
{
    return QStringLiteral("r%1-%2")
        .arg(revision)
        .arg(manifestSha(revision).first(16));
}

QByteArray activeBytes(const int revision)
{
    const QString id = bundleId(revision);
    return QStringLiteral(R"JSON({
        "schemaVersion": 1,
        "bundleId": "%1",
        "path": "releases/%1",
        "revision": %2,
        "generation": %2,
        "manifestSha256": "%3",
        "activatedAt": "2026-07-24T12:00:00+09:00"
    })JSON")
        .arg(id)
        .arg(revision)
        .arg(manifestSha(revision))
        .toUtf8();
}

QString releaseFile(
    const QString &root,
    const int revision,
    const QString &relativePath
)
{
    return QDir(root).filePath(
        QStringLiteral("releases/%1/%2")
            .arg(bundleId(revision), relativePath)
    );
}

bool activateRevision(const QString &root, const int revision)
{
    QSaveFile activeFile(
        QDir(root).filePath(QStringLiteral("active.json"))
    );
    if (!activeFile.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray bytes = activeBytes(revision);
    return activeFile.write(bytes) == bytes.size() && activeFile.commit();
}

QString createValidBundle(
    const QString &root,
    const int revision = 42,
    const QString &mediaTag = QStringLiteral("initial"),
    const QString &layoutMode = QStringLiteral("dashboard"),
    const bool defaultMuted = false,
    const qreal safeLeft = 0.04,
    const bool activate = true
)
{
    const QString id = bundleId(revision);
    const QString release = QDir(root).filePath(
        QStringLiteral("releases/%1").arg(id)
    );
    const QString sha = manifestSha(revision);
    const QByteArray complete = QStringLiteral(R"JSON({
        "schemaVersion": 1,
        "bundleId": "%1",
        "revision": %2,
        "manifestSha256": "%3",
        "completedAt": "2026-07-24T11:59:00+09:00"
    })JSON").arg(id).arg(revision).arg(sha).toUtf8();
    const QByteArray bundle = R"JSON({
        "schemaVersion": 1,
        "display": "display.json",
        "playlist": "playlist.json",
        "schedule": "schedule.json",
        "subtitle": "subtitle.json",
        "activation": {
            "settings": "immediate",
            "playlist": "after_current_item",
            "emergency": "immediate"
        }
    })JSON";
    const QByteArray display = QStringLiteral(R"JSON({
        "schemaVersion": 1,
        "layoutMode": "%1",
        "defaultMediaFit": "fit",
        "safeInsets": {
            "left": %2,
            "top": 0.01,
            "right": 0.02,
            "bottom": 0.03
        },
        "audio": {"muted": %3, "volume": 0.8}
    })JSON")
        .arg(layoutMode)
        .arg(safeLeft)
        .arg(defaultMuted ? QStringLiteral("true") : QStringLiteral("false"))
        .toUtf8();
    const QByteArray playlist = QStringLiteral(R"JSON({
        "schemaVersion": 1,
        "repeat": "all",
        "items": [
            {
                "id": "second",
                "order": 20,
                "kind": "video",
                "path": "media/%1-second.mp4",
                "fit": "crop",
                "muted": true,
                "volume": 0.25
            },
            {
                "id": "first",
                "order": 10,
                "kind": "video",
                "path": "media/%1-first.mp4",
                "fit": null,
                "muted": false,
                "volume": null
            }
        ]
    })JSON").arg(mediaTag).toUtf8();
    const QByteArray schedule = R"JSON({
        "schemaVersion": 1,
        "timezone": "Asia/Seoul",
        "events": [
            {
                "id": "weekly-event",
                "title": "Weekly",
                "location": "2층",
                "daysOfWeek": [5],
                "date": null,
                "startTime": "23:50",
                "endTime": "00:20",
                "endsNextDay": true,
                "priority": 10
            }
        ]
    })JSON";
    const QByteArray subtitle = R"JSON({
        "schemaVersion": 1,
        "messages": [
            {
                "id": "message-1",
                "text": "<b>plain text</b>",
                "durationMs": 1500
            },
            {
                "id": "message-2",
                "text": "second",
                "durationMs": 2500
            }
        ]
    })JSON";

    const bool written =
        writeBytes(
            QDir(release).filePath(QStringLiteral(".manifest.json")),
            manifestBytes(revision)
        )
        && writeBytes(
            QDir(release).filePath(QStringLiteral(".complete")),
            complete
        )
        && writeBytes(
            QDir(release).filePath(QStringLiteral("bundle.json")),
            bundle
        )
        && writeBytes(
            QDir(release).filePath(QStringLiteral("display.json")),
            display
        )
        && writeBytes(
            QDir(release).filePath(QStringLiteral("playlist.json")),
            playlist
        )
        && writeBytes(
            QDir(release).filePath(QStringLiteral("schedule.json")),
            schedule
        )
        && writeBytes(
            QDir(release).filePath(QStringLiteral("subtitle.json")),
            subtitle
        )
        && writeBytes(
            QDir(release).filePath(
                QStringLiteral("media/%1-first.mp4").arg(mediaTag)
            ),
            QByteArrayLiteral("first")
        )
        && writeBytes(
            QDir(release).filePath(
                QStringLiteral("media/%1-second.mp4").arg(mediaTag)
            ),
            QByteArrayLiteral("second")
        )
        && (
            !activate
            || writeBytes(
                QDir(root).filePath(QStringLiteral("active.json")),
                activeBytes(revision)
            )
        );
    return written ? id : QString();
}

QDateTime utc(const QString &text)
{
    return QDateTime::fromString(text, Qt::ISODate).toUTC();
}

}  // namespace

class DisplayCoreTest final : public QObject {
    Q_OBJECT

private slots:
    void geometryUsesFractionalSafeInsetsAndPreservesAspect();
    void playlistRepeatsAndBacksOffFailures();
    void scheduleProjectsRecurringAndDatedSeoulIntervals();
    void floorScheduleUsesFloorRules();
    void clockUsesKoreanTwelveHourHierarchy();
    void bundleParserAcceptsServerCompilerContract();
    void bundleParserAcceptsStructuredRichSubtitles();
    void bundleParserRejectsStretchAndIncompleteIdentity();
    void controllerKeepsLastKnownGoodAfterInvalidActivation();
    void watcherReloadsAfterAtomicActiveReplace();
    void watcherRecoversWhenContentRootAppearsAfterStartup();
    void settingsApplyImmediatelyWhilePlaylistWaitsForBoundary();
    void singleSubtitleDoesNotAnimateAgainWhileUnchanged();
    void hotReloadSequencePreservesPlaybackBoundariesAndFallback();
    void checkedInExampleParses();
    void screenSelectionPrefersEdidThenHdmiOrder();
    void drmScannerRequiresConnectedValidEdid();
};

void DisplayCoreTest::geometryUsesFractionalSafeInsetsAndPreservesAspect()
{
    const QRectF safe = LayoutGeometry::safeRect(
        QSizeF(1000, 500),
        EdgeInsets{0.1, 0.1, 0.1, 0.1}
    );
    QCOMPARE(safe, QRectF(100, 50, 800, 400));

    const QRectF fitted = LayoutGeometry::fitRect(
        QRectF(0, 0, 1000, 1000),
        QSizeF(1920, 1080)
    );
    QCOMPARE(fitted, QRectF(0, 218.75, 1000, 562.5));

    const QRectF crop = LayoutGeometry::cropSourceRect(
        QSizeF(1000, 1000),
        QSizeF(1920, 1080)
    );
    QCOMPARE(crop, QRectF(420, 0, 1080, 1080));
}

void DisplayCoreTest::playlistRepeatsAndBacksOffFailures()
{
    PlaylistNavigator navigator;
    navigator.reset(2);
    QCOMPARE(navigator.begin(0), std::optional<int>(0));

    navigator.reportFailure(0);
    QCOMPARE(navigator.advance(0), std::optional<int>(1));
    navigator.reportFailure(0);
    QVERIFY(!navigator.advance(0).has_value());
    QCOMPARE(navigator.millisecondsUntilNextRetry(0), 1000);

    QCOMPARE(navigator.begin(1000), std::optional<int>(0));
    navigator.reportSuccess();
    QCOMPARE(navigator.advance(1000), std::optional<int>(1));
    navigator.reportSuccess();
    QCOMPARE(navigator.advance(1000), std::optional<int>(0));

    navigator.reset(0);
    QVERIFY(!navigator.begin(0).has_value());
    QVERIFY(!navigator.advance(0).has_value());

    navigator.reset(1);
    QCOMPARE(navigator.begin(0), std::optional<int>(0));
    const QVector<qint64> expectedDelays{
        1'000, 2'000, 4'000, 8'000, 16'000, 30'000, 30'000,
    };
    qint64 nowMs = 0;
    for (const qint64 expectedDelay : expectedDelays) {
        navigator.reportFailure(nowMs);
        QVERIFY(!navigator.advance(nowMs).has_value());
        QCOMPARE(
            navigator.millisecondsUntilNextRetry(nowMs),
            expectedDelay
        );
        nowMs += expectedDelay;
        QCOMPARE(navigator.begin(nowMs), std::optional<int>(0));
    }
}

void DisplayCoreTest::scheduleProjectsRecurringAndDatedSeoulIntervals()
{
    const ScheduleItem overnight{
        QStringLiteral("overnight"),
        QStringLiteral("Overnight"),
        QStringLiteral("2층"),
        {5},
        std::nullopt,
        QTime(23, 50),
        QTime(0, 20),
        true,
        20,
    };
    const ScheduleItem dated{
        QStringLiteral("dated"),
        QStringLiteral("Dated"),
        QStringLiteral("2F"),
        {},
        QDate(2026, 7, 25),
        QTime(1, 0),
        QTime(2, 0),
        false,
        0,
    };
    const ScheduleItem lowerPriorityActive{
        QStringLiteral("lower"),
        QStringLiteral("Lower"),
        QStringLiteral("3F"),
        {5},
        std::nullopt,
        QTime(23, 55),
        QTime(0, 15),
        true,
        0,
    };
    const ScheduleItem tomorrow{
        QStringLiteral("tomorrow"),
        QStringLiteral("Tomorrow"),
        QStringLiteral("4F"),
        {},
        QDate(2026, 7, 26),
        QTime(0, 30),
        QTime(1, 0),
        false,
        100,
    };

    QVector<ScheduleRow> rows = ScheduleProjection::at(
        {overnight, lowerPriorityActive, dated, tomorrow},
        utc(QStringLiteral("2026-07-25T00:05:00+09:00"))
    );
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows[0].item.id, QStringLiteral("overnight"));
    QVERIFY(rows[0].status == ScheduleStatus::Active);
    QCOMPARE(rows[1].item.id, QStringLiteral("lower"));
    QVERIFY(rows[1].status == ScheduleStatus::Active);
    QCOMPARE(rows[2].item.id, QStringLiteral("dated"));
    QVERIFY(rows[2].status == ScheduleStatus::Upcoming);

    rows = ScheduleProjection::at(
        {overnight, dated, tomorrow},
        utc(QStringLiteral("2026-07-24T23:50:00+09:00"))
    );
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].item.id, QStringLiteral("overnight"));
    QVERIFY(rows[0].status == ScheduleStatus::Active);

    rows = ScheduleProjection::at(
        {overnight, lowerPriorityActive, dated, tomorrow},
        utc(QStringLiteral("2026-07-25T00:20:00+09:00"))
    );
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].item.id, QStringLiteral("dated"));

    ScheduleListModel model;
    model.setItems({dated});
    model.refresh(utc(QStringLiteral("2026-07-25T00:20:00+09:00")));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(
        model.data(model.index(0), ScheduleListModel::DayRole).toString(),
        QStringLiteral("7월 25일(토)")
    );

    const ScheduleItem sunday{
        QStringLiteral("sunday"),
        QStringLiteral("Sunday"),
        QStringLiteral("2F"),
        {7},
        std::nullopt,
        QTime(9, 0),
        QTime(11, 0),
        false,
        0,
    };
    QVERIFY(ScheduleProjection::at(
        {sunday},
        utc(QStringLiteral("2026-07-25T10:00:00+09:00"))
    ).isEmpty());

    rows = ScheduleProjection::at(
        {sunday},
        utc(QStringLiteral("2026-07-26T08:59:59+09:00"))
    );
    QCOMPARE(rows.size(), 1);
    QVERIFY(rows[0].status == ScheduleStatus::Upcoming);
    rows = ScheduleProjection::at(
        {sunday},
        utc(QStringLiteral("2026-07-26T09:00:00+09:00"))
    );
    QCOMPARE(rows.size(), 1);
    QVERIFY(rows[0].status == ScheduleStatus::Active);
    QVERIFY(ScheduleProjection::at(
        {sunday},
        utc(QStringLiteral("2026-07-26T11:00:00+09:00"))
    ).isEmpty());

    const ScheduleItem monday{
        QStringLiteral("monday"),
        QStringLiteral("Monday"),
        QStringLiteral("3F"),
        {1},
        std::nullopt,
        QTime(10, 0),
        QTime(11, 0),
        false,
        0,
    };
    rows = ScheduleProjection::at(
        {monday},
        utc(QStringLiteral("2026-07-27T09:00:00+09:00"))
    );
    QCOMPARE(rows.size(), 1);
    QVERIFY(rows[0].status == ScheduleStatus::Upcoming);

    const ScheduleItem sundayOvernight{
        QStringLiteral("sunday-overnight"),
        QStringLiteral("Sunday Overnight"),
        QStringLiteral("2층"),
        {7},
        std::nullopt,
        QTime(23, 30),
        QTime(1, 0),
        true,
        0,
    };
    rows = ScheduleProjection::at(
        {sundayOvernight},
        utc(QStringLiteral("2026-07-27T00:30:00+09:00"))
    );
    QCOMPARE(rows.size(), 1);
    QVERIFY(rows[0].status == ScheduleStatus::Active);
    QVERIFY(ScheduleProjection::at(
        {sundayOvernight},
        utc(QStringLiteral("2026-07-27T01:00:00+09:00"))
    ).isEmpty());
}

void DisplayCoreTest::floorScheduleUsesFloorRules()
{
    const ScheduleItem first{
        QStringLiteral("first"),
        QStringLiteral("오전 행사"),
        QStringLiteral("2층 4층"),
        {7},
        std::nullopt,
        QTime(10, 0),
        QTime(11, 0),
        false,
        0,
    };
    const ScheduleItem next{
        QStringLiteral("next"),
        QStringLiteral("정오 행사"),
        QStringLiteral("2F"),
        {7},
        std::nullopt,
        QTime(12, 0),
        QTime(13, 0),
        false,
        0,
    };
    const ScheduleItem closeNext{
        QStringLiteral("close"),
        QStringLiteral("바로 다음 행사"),
        QStringLiteral("F3"),
        {7},
        std::nullopt,
        QTime(11, 20),
        QTime(12, 0),
        false,
        0,
    };
    const ScheduleItem thirdFirst{
        QStringLiteral("third-first"),
        QStringLiteral("3층 이전 행사"),
        QStringLiteral("3층"),
        {7},
        std::nullopt,
        QTime(10, 0),
        QTime(11, 0),
        false,
        0,
    };
    const ScheduleItem ignoredFloorOne{
        QStringLiteral("floor-one"),
        QStringLiteral("1층은 표시하지 않음"),
        QStringLiteral("1층"),
        {7},
        std::nullopt,
        QTime(10, 0),
        QTime(14, 0),
        false,
        100,
    };

    const auto floor2 = [&](
        const QString &localTime
    ) -> FloorScheduleRow {
        const QVector<FloorScheduleRow> rows =
            ScheduleProjection::forFloors(
                {first, next, ignoredFloorOne},
                utc(QStringLiteral("2026-07-26T%1:00+09:00").arg(localTime))
            );
        return rows.value(2);
    };

    QCOMPARE(floor2(QStringLiteral("09:29")).floor, 2);
    QCOMPARE(
        floor2(QStringLiteral("09:29")).status,
        FloorScheduleStatus::Inactive
    );
    QVERIFY(floor2(QStringLiteral("09:29")).item.has_value());
    QCOMPARE(
        floor2(QStringLiteral("09:29")).item->id,
        QStringLiteral("first")
    );
    QCOMPARE(
        floor2(QStringLiteral("09:30")).status,
        FloorScheduleStatus::Upcoming
    );
    QCOMPARE(
        floor2(QStringLiteral("09:50")).status,
        FloorScheduleStatus::StartingSoon
    );
    QCOMPARE(
        floor2(QStringLiteral("10:00")).status,
        FloorScheduleStatus::Active
    );
    QCOMPARE(
        floor2(QStringLiteral("11:29")).status,
        FloorScheduleStatus::Ended
    );
    QCOMPARE(
        floor2(QStringLiteral("11:30")).status,
        FloorScheduleStatus::Upcoming
    );

    const ScheduleItem lateMorningWorship{
        QStringLiteral("late-morning"),
        QStringLiteral("일요일 2차 행사"),
        QStringLiteral("2층 대행사실"),
        {7},
        std::nullopt,
        QTime(11, 0),
        QTime(12, 30),
        false,
        0,
    };
    const QVector<FloorScheduleRow> afterWorshipRows =
        ScheduleProjection::forFloors(
            {lateMorningWorship},
            utc(QStringLiteral("2026-07-26T13:00:00+09:00"))
        );
    QCOMPARE(afterWorshipRows[2].floor, 2);
    QCOMPARE(
        afterWorshipRows[2].status,
        FloorScheduleStatus::Ended
    );
    QCOMPARE(
        afterWorshipRows[2].item->id,
        QStringLiteral("late-morning")
    );

    const QVector<FloorScheduleRow> closeRows =
        ScheduleProjection::forFloors(
            {thirdFirst, closeNext},
            utc(QStringLiteral("2026-07-26T11:00:00+09:00"))
        );
    QCOMPARE(closeRows[1].floor, 3);
    QCOMPARE(closeRows[1].item->id, QStringLiteral("close"));
    QCOMPARE(closeRows[1].status, FloorScheduleStatus::Upcoming);

    FloorScheduleModel model;
    model.setItems({first, next});
    model.refresh(utc(QStringLiteral("2026-07-26T09:55:00+09:00")));
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(
        model.data(model.index(2), FloorScheduleModel::StatusCodeRole)
            .toString(),
        QStringLiteral("starting_soon")
    );
    model.refresh(utc(QStringLiteral("2026-07-26T00:00:00+09:00")));
    QCOMPARE(
        model.data(model.index(2), FloorScheduleModel::StatusCodeRole)
            .toString(),
        QStringLiteral("inactive")
    );
    QCOMPARE(
        model.data(model.index(2), FloorScheduleModel::StatusLabelRole)
            .toString(),
        QStringLiteral("오늘 일정")
    );

    const ScheduleItem tomorrow{
        QStringLiteral("tomorrow"),
        QStringLiteral("자정 설명회"),
        QStringLiteral("4층"),
        {1},
        std::nullopt,
        QTime(0, 10),
        QTime(1, 0),
        false,
        0,
    };
    const QVector<FloorScheduleRow> nextDayRows =
        ScheduleProjection::forFloors(
            {tomorrow},
            utc(QStringLiteral("2026-07-26T23:40:00+09:00"))
        );
    QCOMPARE(nextDayRows[0].floor, 4);
    QVERIFY(nextDayRows[0].item.has_value());
    QCOMPARE(nextDayRows[0].item->id, QStringLiteral("tomorrow"));
    QCOMPARE(nextDayRows[0].status, FloorScheduleStatus::Upcoming);
}

void DisplayCoreTest::clockUsesKoreanTwelveHourHierarchy()
{
    QCOMPARE(
        ClockFormatter::korean12Hour(
            utc(QStringLiteral("2026-07-25T00:05:09+09:00"))
        ),
        (ClockParts{
            QStringLiteral("오전"),
            QStringLiteral("12:05:09"),
            QStringLiteral("7월 25일 토요일"),
        })
    );
    QCOMPARE(
        ClockFormatter::korean12Hour(
            utc(QStringLiteral("2026-07-25T13:07:08+09:00"))
        ).time,
        QStringLiteral("01:07:08")
    );
    QCOMPARE(
        ClockFormatter::korean12Hour(
            utc(QStringLiteral("2026-07-25T13:07:08+09:00"))
        ).period,
        QStringLiteral("오후")
    );
}

void DisplayCoreTest::bundleParserAcceptsServerCompilerContract()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString expectedId = createValidBundle(directory.path());
    QVERIFY(!expectedId.isEmpty());

    const BundleParseResult result = BundleParser::loadActive(directory.path());
    QVERIFY2(result.isValid(), qPrintable(result.error));
    const BundleSnapshot &snapshot = *result.snapshot;
    QCOMPARE(snapshot.bundleId, expectedId);
    QVERIFY(snapshot.displayMode == DisplayMode::Dashboard);
    QVERIFY(snapshot.defaultMediaFit == MediaFit::Fit);
    QVERIFY(!snapshot.defaultMuted);
    QCOMPARE(snapshot.defaultVolume, 0.8);
    QVERIFY(
        snapshot.playlistActivation == ActivationPolicy::AfterCurrentItem
    );
    QCOMPARE(snapshot.safeInsets, (EdgeInsets{0.01, 0.02, 0.03, 0.04}));
    QCOMPARE(snapshot.playlist.size(), 2);
    QCOMPARE(snapshot.playlist[0].id, QStringLiteral("first"));
    QVERIFY(!snapshot.playlist[0].fit.has_value());
    QCOMPARE(snapshot.playlist[1].id, QStringLiteral("second"));
    QVERIFY(snapshot.playlist[1].fit == MediaFit::Crop);
    QCOMPARE(snapshot.schedule.size(), 1);
    QCOMPARE(snapshot.schedule[0].daysOfWeek, QVector<int>({5}));
    QVERIFY(snapshot.schedule[0].endsNextDay);
    QCOMPARE(snapshot.subtitles.size(), 2);
    QCOMPARE(snapshot.subtitles[0].durationMs, 1500);
    QCOMPARE(
        snapshot.subtitles[0].text,
        QStringLiteral("<b>plain text</b>")
    );
    QCOMPARE(
        snapshot.subtitles[0].richText,
        QStringLiteral("&lt;b&gt;plain text&lt;/b&gt;")
    );
}

void DisplayCoreTest::bundleParserAcceptsStructuredRichSubtitles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString id = createValidBundle(directory.path());
    QVERIFY(!id.isEmpty());
    const QString subtitlePath = QDir(directory.path()).filePath(
        QStringLiteral("releases/%1/subtitle.json").arg(id)
    );
    QVERIFY(writeBytes(
        subtitlePath,
        R"JSON({
            "schemaVersion": 2,
            "messages": [{
                "id": "rich",
                "durationMs": 6000,
                "effect": "slide_up",
                "fontSizePx": 72,
                "transitionMs": 1200,
                "lines": [
                    {"spans": [
                        {
                            "text": "<환영>",
                            "size": "large",
                            "marks": ["bold", "underline"]
                        },
                        {
                            "text": "합니다",
                            "size": "normal",
                            "marks": ["italic", "strike"]
                        }
                    ]},
                    {"spans": [
                        {
                            "text": "안내 센터",
                            "size": "small",
                            "marks": []
                        }
                    ]}
                ]
            }]
        })JSON"
    ));

    const BundleParseResult result = BundleParser::loadActive(directory.path());
    QVERIFY2(result.isValid(), qPrintable(result.error));
    QCOMPARE(result.snapshot->subtitles.size(), 1);
    const SubtitleMessage &message = result.snapshot->subtitles[0];
    QCOMPARE(message.text, QStringLiteral("<환영>합니다\n안내 센터"));
    QVERIFY(message.richText.contains(QStringLiteral("&lt;환영&gt;")));
    QVERIFY(message.richText.contains(QStringLiteral("<b>")));
    QVERIFY(message.richText.contains(QStringLiteral("<u>")));
    QVERIFY(message.richText.contains(QStringLiteral("<br/>")));
    QVERIFY(!message.richText.contains(QStringLiteral("<환영>")));
    QCOMPARE(message.effect, QStringLiteral("slide_up"));
    QCOMPARE(message.fontSizePx, 72);
    QCOMPARE(message.transitionMs, 1200);
}

void DisplayCoreTest::bundleParserRejectsStretchAndIncompleteIdentity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString id = createValidBundle(directory.path());
    QVERIFY(!id.isEmpty());

    const QString displayPath = QDir(directory.path()).filePath(
        QStringLiteral("releases/%1/display.json").arg(id)
    );
    QVERIFY(writeBytes(
        displayPath,
        R"JSON({
            "schemaVersion": 1,
            "layoutMode": "dashboard",
            "defaultMediaFit": "stretch",
            "safeInsets": {"top": 0, "right": 0, "bottom": 0, "left": 0},
            "audio": {"muted": false, "volume": 1}
        })JSON"
    ));
    BundleParseResult result = BundleParser::loadActive(directory.path());
    QVERIFY(!result.isValid());
    QVERIFY(result.error.contains(QStringLiteral("fit or crop")));

    QVERIFY(createValidBundle(directory.path()).size() > 0);
    QVERIFY(writeBytes(
        QDir(directory.path()).filePath(
            QStringLiteral("releases/%1/.complete").arg(id)
        ),
        QByteArrayLiteral("{}")
    ));
    result = BundleParser::loadActive(directory.path());
    QVERIFY(!result.isValid());
    QVERIFY(result.error.contains(QStringLiteral("completion identity")));
}

void DisplayCoreTest::controllerKeepsLastKnownGoodAfterInvalidActivation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString id = createValidBundle(directory.path());
    QVERIFY(!id.isEmpty());

    DisplayController controller(directory.path());
    QTRY_COMPARE(controller.bundleId(), id);
    QVERIFY(writeBytes(
        QDir(directory.path()).filePath(QStringLiteral("active.json")),
        QByteArrayLiteral("{}")
    ));

    auto *watcher = controller.findChild<ActiveBundleWatcher *>();
    QVERIFY(watcher != nullptr);
    watcher->reloadNow();

    QCOMPARE(controller.bundleId(), id);
    QVERIFY(controller.statusText().contains(QStringLiteral("이전 화면 유지")));
}

void DisplayCoreTest::watcherReloadsAfterAtomicActiveReplace()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString firstId = createValidBundle(directory.path());
    QVERIFY(!firstId.isEmpty());

    DisplayController controller(directory.path());
    QTRY_COMPARE(controller.bundleId(), firstId);

    const QString secondId = createValidBundle(
        directory.path(),
        43,
        QStringLiteral("atomic"),
        QStringLiteral("dashboard"),
        false,
        0.04,
        false
    );
    QVERIFY(!secondId.isEmpty());

    QSaveFile activeFile(
        QDir(directory.path()).filePath(QStringLiteral("active.json"))
    );
    QVERIFY(activeFile.open(QIODevice::WriteOnly));
    const QByteArray secondActive = activeBytes(43);
    QCOMPARE(activeFile.write(secondActive), secondActive.size());
    QVERIFY(activeFile.commit());

    QTRY_COMPARE(controller.bundleId(), secondId);
    QVERIFY(controller.currentMediaSource().toLocalFile().endsWith(
        QStringLiteral("initial-first.mp4")
    ));
    controller.mediaEnded();
    QVERIFY(controller.currentMediaSource().toLocalFile().endsWith(
        QStringLiteral("atomic-first.mp4")
    ));
}

void DisplayCoreTest::watcherRecoversWhenContentRootAppearsAfterStartup()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString parent =
        QDir(directory.path()).filePath(QStringLiteral("late"));
    QVERIFY(QDir().mkpath(parent));
    const QString root = QDir(parent).filePath(QStringLiteral("Release"));

    ActiveBundleWatcher watcher(root);
    QSignalSpy failed(&watcher, &ActiveBundleWatcher::loadFailed);
    QVERIFY(failed.isValid());
    QString emittedId;
    int readyCount = 0;
    connect(
        &watcher,
        &ActiveBundleWatcher::snapshotReady,
        this,
        [&](const BundleSnapshot &snapshot) {
            emittedId = snapshot.bundleId;
            ++readyCount;
        }
    );

    QTRY_COMPARE(failed.count(), 1);
    const QString id = createValidBundle(root);
    QVERIFY(!id.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(readyCount, 1, 2'000);
    QCOMPARE(emittedId, id);
}

void DisplayCoreTest::settingsApplyImmediatelyWhilePlaylistWaitsForBoundary()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString firstId = createValidBundle(directory.path());
    QVERIFY(!firstId.isEmpty());

    DisplayController controller(directory.path());
    QTRY_COMPARE(controller.bundleId(), firstId);
    const QUrl firstSource = controller.currentMediaSource();
    QVERIFY(firstSource.toLocalFile().endsWith(
        QStringLiteral("initial-first.mp4")
    ));
    QVERIFY(!controller.currentMediaMuted());

    const QString secondId = createValidBundle(
        directory.path(),
        43,
        QStringLiteral("next"),
        QStringLiteral("fullscreen_media"),
        true,
        0.08
    );
    QVERIFY(!secondId.isEmpty());
    auto *watcher = controller.findChild<ActiveBundleWatcher *>();
    QVERIFY(watcher != nullptr);
    watcher->reloadNow();

    QCOMPARE(controller.bundleId(), secondId);
    QCOMPARE(controller.displayMode(), QStringLiteral("fullscreen_media"));
    QCOMPARE(controller.safeLeft(), 0.08);
    QVERIFY(controller.currentMediaMuted());
    QCOMPARE(controller.currentMediaSource(), firstSource);
    QVERIFY(controller.statusText().contains(QStringLiteral("현재 항목 종료")));

    controller.mediaEnded();
    QVERIFY(controller.currentMediaSource().toLocalFile().endsWith(
        QStringLiteral("next-first.mp4")
    ));
}

void DisplayCoreTest::singleSubtitleDoesNotAnimateAgainWhileUnchanged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString id = createValidBundle(directory.path());
    QVERIFY(!id.isEmpty());
    QVERIFY(writeBytes(
        QDir(directory.path()).filePath(
            QStringLiteral("releases/%1/subtitle.json").arg(id)
        ),
        R"JSON({
            "schemaVersion": 1,
            "messages": [{
                "id": "only",
                "text": "한 줄 공지",
                "durationMs": 1000
            }]
        })JSON"
    ));

    DisplayController controller(directory.path());
    QSignalSpy subtitleChanges(
        &controller,
        &DisplayController::subtitleChanged
    );
    QVERIFY(subtitleChanges.isValid());
    QTRY_COMPARE(controller.subtitleText(), QStringLiteral("한 줄 공지"));
    QCOMPARE(subtitleChanges.count(), 1);
    QTest::qWait(1'200);
    QCOMPARE(subtitleChanges.count(), 1);
}

void DisplayCoreTest::hotReloadSequencePreservesPlaybackBoundariesAndFallback()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QByteArray emptyPlaylist = R"JSON({
        "schemaVersion": 1,
        "repeat": "all",
        "items": []
    })JSON";
    const QString emptyId = createValidBundle(directory.path(), 50);
    QVERIFY(!emptyId.isEmpty());
    QVERIFY(writeBytes(
        releaseFile(directory.path(), 50, QStringLiteral("playlist.json")),
        emptyPlaylist
    ));

    DisplayController controller(directory.path());
    QTRY_COMPARE(controller.bundleId(), emptyId);
    QVERIFY(controller.currentMediaSource().isEmpty());
    QCOMPARE(
        controller.fallbackMessage(),
        QStringLiteral("재생할 콘텐츠가 없습니다")
    );

    const QString subtitleId = createValidBundle(
        directory.path(),
        51,
        QStringLiteral("subtitle-only"),
        QStringLiteral("dashboard"),
        false,
        0.04,
        false
    );
    QVERIFY(!subtitleId.isEmpty());
    QVERIFY(writeBytes(
        releaseFile(directory.path(), 51, QStringLiteral("playlist.json")),
        emptyPlaylist
    ));
    QVERIFY(writeBytes(
        releaseFile(directory.path(), 51, QStringLiteral("subtitle.json")),
        R"JSON({
            "schemaVersion": 1,
            "messages": [{
                "id": "notice",
                "text": "subtitle-only revision",
                "durationMs": 5000
            }]
        })JSON"
    ));
    QVERIFY(activateRevision(directory.path(), 51));
    auto *watcher = controller.findChild<ActiveBundleWatcher *>();
    QVERIFY(watcher != nullptr);
    watcher->reloadNow();
    QCOMPARE(controller.bundleId(), subtitleId);
    QCOMPARE(controller.subtitleText(), QStringLiteral("subtitle-only revision"));
    QVERIFY(controller.currentMediaSource().isEmpty());

    const QString firstVideoId = createValidBundle(
        directory.path(),
        52,
        QStringLiteral("first-video"),
        QStringLiteral("dashboard"),
        false,
        0.04,
        false
    );
    QVERIFY(!firstVideoId.isEmpty());
    QVERIFY(activateRevision(directory.path(), 52));
    watcher->reloadNow();
    QCOMPARE(controller.bundleId(), firstVideoId);
    QVERIFY(controller.currentMediaSource().toLocalFile().endsWith(
        QStringLiteral("first-video-first.mp4")
    ));
    const QUrl playingSource = controller.currentMediaSource();
    controller.mediaPlaying();

    const QString settingsId = createValidBundle(
        directory.path(),
        53,
        QStringLiteral("replacement"),
        QStringLiteral("fullscreen_media"),
        true,
        0.08,
        false
    );
    QVERIFY(!settingsId.isEmpty());
    const QDate today = QDateTime::currentDateTimeUtc()
                            .toTimeZone(
                                QTimeZone(QByteArrayLiteral("Asia/Seoul"))
                            )
                            .date();
    const QByteArray currentSchedule = QStringLiteral(R"JSON({
        "schemaVersion": 1,
        "timezone": "Asia/Seoul",
        "events": [{
            "id": "today",
            "title": "Today",
            "location": "2F",
            "daysOfWeek": null,
            "date": "%1",
            "startTime": "00:00",
            "endTime": "23:59",
            "endsNextDay": false,
            "priority": 0
        }]
    })JSON").arg(today.toString(Qt::ISODate)).toUtf8();
    QVERIFY(writeBytes(
        releaseFile(directory.path(), 53, QStringLiteral("schedule.json")),
        currentSchedule
    ));
    QVERIFY(activateRevision(directory.path(), 53));
    watcher->reloadNow();
    QCOMPARE(controller.bundleId(), settingsId);
    QCOMPARE(controller.displayMode(), QStringLiteral("fullscreen_media"));
    QVERIFY(controller.currentMediaMuted());
    QCOMPARE(controller.scheduleModel()->rowCount(), 3);
    QCOMPARE(controller.currentMediaSource(), playingSource);

    controller.mediaEnded();
    QVERIFY(controller.currentMediaSource().toLocalFile().endsWith(
        QStringLiteral("replacement-first.mp4")
    ));
    controller.mediaPlaying();
    controller.mediaFailed(QStringLiteral("decoder detail must not reach UI"));
    QVERIFY(controller.currentMediaSource().toLocalFile().endsWith(
        QStringLiteral("replacement-second.mp4")
    ));
    controller.mediaPlaying();
    controller.mediaFailed(QStringLiteral("decoder detail must not reach UI"));
    QVERIFY(controller.currentMediaSource().isEmpty());
    QCOMPARE(
        controller.fallbackMessage(),
        QStringLiteral(
            "영상을 재생할 수 없습니다 · 잠시 후 다시 시도합니다"
        )
    );
    QVERIFY(!controller.fallbackMessage().contains(QStringLiteral("decoder")));

    QVERIFY(writeBytes(
        QDir(directory.path()).filePath(QStringLiteral("active.json")),
        QByteArrayLiteral("{}")
    ));
    watcher->reloadNow();
    QCOMPARE(controller.bundleId(), settingsId);
    QVERIFY(controller.statusText().contains(QStringLiteral("이전 화면 유지")));
}

void DisplayCoreTest::checkedInExampleParses()
{
    const QString root = QDir(QStringLiteral(DISPLAY_CLIENT_SOURCE_DIR))
                             .filePath(
                                 QStringLiteral("examples/content-root")
                             );
    const BundleParseResult result = BundleParser::loadActive(root);
    QVERIFY2(result.isValid(), qPrintable(result.error));
    QVERIFY(result.snapshot->bundleId.startsWith(QStringLiteral("r1-")));
}

void DisplayCoreTest::screenSelectionPrefersEdidThenHdmiOrder()
{
    const QVector<PhysicalOutput> outputs{
        {
            QStringLiteral("HDMI-A-2"),
            QStringLiteral("edid-two"),
            QStringLiteral("ABC"),
            QStringLiteral("Panel Two"),
            QStringLiteral("2"),
        },
        {
            QStringLiteral("HDMI-A-1"),
            QStringLiteral("edid-one"),
            QStringLiteral("ABC"),
            QStringLiteral("Panel One"),
            QStringLiteral("1"),
        },
    };
    QCOMPARE(ScreenSelector::preferredOutputIndex(outputs, {}), 1);
    QCOMPARE(
        ScreenSelector::preferredOutputIndex(
            outputs,
            QStringLiteral("edid-two")
        ),
        0
    );

    const QVector<ScreenDescriptor> screens{
        {QStringLiteral("Pi Connect Virtual"), {}, {}, {}},
        {
            QStringLiteral("HDMI-2"),
            QStringLiteral("ABC"),
            QStringLiteral("Panel Two"),
            QStringLiteral("2"),
        },
    };
    QCOMPARE(ScreenSelector::matchingScreenIndex(outputs[0], screens), 1);
    QCOMPARE(
        ScreenSelector::matchingScreenIndex(
            outputs[0],
            {
                {
                    QStringLiteral("Pi Connect Virtual"),
                    QStringLiteral("ABC"),
                    QStringLiteral("Panel Two"),
                    QStringLiteral("2"),
                },
            }
        ),
        -1
    );
}

void DisplayCoreTest::drmScannerRequiresConnectedValidEdid()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString connector = QDir(directory.path()).filePath(
        QStringLiteral("card0-HDMI-A-1")
    );
    QVERIFY(writeBytes(
        QDir(connector).filePath(QStringLiteral("status")),
        QByteArrayLiteral("connected\n")
    ));

    QByteArray edid(128, '\0');
    const QByteArray header = QByteArray::fromHex("00ffffffffffff00");
    std::copy(header.begin(), header.end(), edid.begin());
    edid[8] = static_cast<char>(0x4c);
    edid[9] = static_cast<char>(0x2d);
    edid[10] = 0x34;
    edid[11] = 0x12;
    quint8 checksum = 0;
    for (int index = 0; index < 127; ++index) {
        checksum = static_cast<quint8>(
            checksum + static_cast<quint8>(edid[index])
        );
    }
    edid[127] = static_cast<char>(0U - checksum);
    QVERIFY(writeBytes(
        QDir(connector).filePath(QStringLiteral("edid")),
        edid
    ));

    const QVector<PhysicalOutput> outputs =
        DrmOutputScanner::scan(directory.path());
    QCOMPARE(outputs.size(), 1);
    QCOMPARE(outputs[0].connectorName, QStringLiteral("HDMI-A-1"));
    QCOMPARE(outputs[0].edidSha256.size(), 64);

    QVERIFY(writeBytes(
        QDir(connector).filePath(QStringLiteral("status")),
        QByteArrayLiteral("disconnected\n")
    ));
    QVERIFY(DrmOutputScanner::scan(directory.path()).isEmpty());
}

QTEST_GUILESS_MAIN(DisplayCoreTest)

#include "test_display_core.moc"
