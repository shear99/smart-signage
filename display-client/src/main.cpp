#include "display_controller.h"
#include "screen_coordinator.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <QVariant>

using signage::display::DisplayController;
using signage::display::ScreenCoordinator;

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("signage-display-client")
    );
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral(
            "Read-only Signage signage client for a physical DRM display"
        )
    );
    parser.addHelpOption();
    parser.addVersionOption();

    const QString defaultContentRoot = QDir::current().filePath("runtime/content");
    const QCommandLineOption contentRootOption(
        {QStringLiteral("c"), QStringLiteral("content-root")},
        QStringLiteral(
            "Directory whose active.json points at an immutable release"
        ),
        QStringLiteral("path"),
        defaultContentRoot
    );
    const QCommandLineOption preferredEdidOption(
        QStringLiteral("preferred-edid-sha256"),
        QStringLiteral(
            "Prefer the connected physical output with this EDID SHA-256"
        ),
        QStringLiteral("hex")
    );
    const QCommandLineOption drmRootOption(
        QStringLiteral("drm-root"),
        QStringLiteral("DRM sysfs root (used by tests and lab images)"),
        QStringLiteral("path"),
        QStringLiteral("/sys/class/drm")
    );
    const QCommandLineOption smokeTestOption(
        QStringLiteral("smoke-test"),
        QStringLiteral(
            "Load the embedded QML graph and exit before selecting a screen"
        )
    );
    parser.addOption(contentRootOption);
    parser.addOption(preferredEdidOption);
    parser.addOption(drmRootOption);
    parser.addOption(smokeTestOption);
    parser.process(application);

    DisplayController controller(
        QDir::cleanPath(parser.value(contentRootOption))
    );

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("displayController"),
        &controller
    );
    engine.rootContext()->setContextProperty(
        QStringLiteral("displaySmokeTest"),
        parser.isSet(smokeTestOption)
    );
    engine.rootContext()->setContextProperty(
        QStringLiteral("displayFontFamily"),
        QGuiApplication::font().family()
    );
    engine.load(QUrl(QStringLiteral(
        "qrc:/qt/qml/Signage/Display/Main.qml"
    )));
    if (engine.rootObjects().isEmpty()) {
        return 2;
    }

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr) {
        qCritical("Main QML object is not a QQuickWindow");
        return 3;
    }
    if (parser.isSet(smokeTestOption)) {
        window->resize(1920, 1080);
        QObject *mediaPlayer = window->findChild<QObject *>(
            QStringLiteral("mediaPlayer")
        );
        QObject *scheduleList = window->findChild<QObject *>(
            QStringLiteral("scheduleList")
        );
        if (mediaPlayer == nullptr || scheduleList == nullptr) {
            qCritical("Smoke test could not find required QML objects");
            return 4;
        }

        QTimer smokePoll;
        smokePoll.setInterval(20);
        QObject::connect(&smokePoll, &QTimer::timeout, &application, [&]() {
            const QUrl source =
                mediaPlayer->property("source").value<QUrl>();
            const QUrl expected = controller.currentMediaSource();
            const int qmlScheduleCount =
                scheduleList->property("count").toInt();
            if (
                !expected.isEmpty()
                && source == expected
                && qmlScheduleCount
                    == controller.scheduleModel()->rowCount()
            ) {
                application.quit();
            }
        });
        QTimer smokeTimeout;
        smokeTimeout.setSingleShot(true);
        smokeTimeout.setInterval(3'000);
        QObject::connect(
            &smokeTimeout,
            &QTimer::timeout,
            &application,
            &QCoreApplication::quit
        );
        smokePoll.start();
        smokeTimeout.start();

        const int exitCode = application.exec();
        if (exitCode != 0) {
            return exitCode;
        }

        const QUrl actualSource =
            mediaPlayer->property("source").value<QUrl>();
        const QUrl expectedSource = controller.currentMediaSource();
        if (expectedSource.isEmpty() || actualSource != expectedSource) {
            qCritical().noquote()
                << "Smoke test initial media binding mismatch:"
                << actualSource << "!=" << expectedSource;
            return 5;
        }
        const int qmlScheduleCount =
            scheduleList->property("count").toInt();
        const int controllerScheduleCount =
            controller.scheduleModel()->rowCount();
        if (qmlScheduleCount != controllerScheduleCount) {
            qCritical()
                << "Smoke test schedule model mismatch:"
                << qmlScheduleCount << "!=" << controllerScheduleCount;
            return 6;
        }
        return 0;
    }

    ScreenCoordinator screenCoordinator(
        window,
        parser.value(preferredEdidOption),
        parser.value(drmRootOption)
    );
    screenCoordinator.start();

    return application.exec();
}
