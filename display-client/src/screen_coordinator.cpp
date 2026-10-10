#include "screen_coordinator.h"

#include "screen_selector.h"

#include <QDebug>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>

namespace signage::display {

ScreenCoordinator::ScreenCoordinator(
    QQuickWindow *window,
    QString preferredEdidSha256,
    QString drmRoot,
    QObject *parent
)
    : QObject(parent)
    , m_window(window)
    , m_preferredEdidSha256(std::move(preferredEdidSha256))
    , m_drmRoot(std::move(drmRoot))
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(100);
    connect(&m_debounce, &QTimer::timeout, this, &ScreenCoordinator::reselect);

    connect(
        qGuiApp,
        &QGuiApplication::screenAdded,
        this,
        [this](QScreen *) {
            reconnectScreenSignals();
            scheduleReselect();
        }
    );
    connect(
        qGuiApp,
        &QGuiApplication::screenRemoved,
        this,
        [this](QScreen *) {
            reconnectScreenSignals();
            scheduleReselect();
        }
    );
    connect(
        m_window,
        &QQuickWindow::screenChanged,
        this,
        [this](QScreen *) {
            scheduleReselect();
        }
    );
}

void ScreenCoordinator::start()
{
    reconnectScreenSignals();
    scheduleReselect();
}

void ScreenCoordinator::scheduleReselect()
{
    m_debounce.start();
}

void ScreenCoordinator::reselect()
{
    const QVector<PhysicalOutput> outputs = DrmOutputScanner::scan(m_drmRoot);
    const int outputIndex = ScreenSelector::preferredOutputIndex(
        outputs,
        m_preferredEdidSha256
    );
    if (outputIndex < 0) {
        m_window->hide();
        qWarning("No connected physical DRM output with a valid EDID");
        return;
    }

    const QList<QScreen *> applicationScreens = QGuiApplication::screens();
    QVector<ScreenDescriptor> screens;
    screens.reserve(applicationScreens.size());
    for (const QScreen *screen : applicationScreens) {
        screens.push_back(
            {
                screen->name(),
                screen->manufacturer(),
                screen->model(),
                screen->serialNumber(),
            }
        );
    }
    const int screenIndex = ScreenSelector::matchingScreenIndex(
        outputs[outputIndex],
        screens
    );
    if (screenIndex < 0 || screenIndex >= applicationScreens.size()) {
        m_window->hide();
        qWarning("Connected DRM output does not map to a non-virtual QScreen");
        return;
    }

    if (m_preferredEdidSha256.isEmpty()) {
        m_preferredEdidSha256 = outputs[outputIndex].edidSha256;
    }

    QScreen *screen = applicationScreens[screenIndex];
    qInfo().noquote()
        << "Display physical screen selected"
        << "name=" << screen->name()
        << "geometry=" << screen->geometry()
        << "device_pixel_ratio=" << screen->devicePixelRatio()
        << "drm_output=" << outputs[outputIndex].connectorName;
    m_window->setScreen(screen);
    m_window->setGeometry(screen->geometry());
    m_window->setCursor(Qt::BlankCursor);
    m_window->showFullScreen();
}

void ScreenCoordinator::reconnectScreenSignals()
{
    for (const QMetaObject::Connection &connection : m_screenConnections) {
        disconnect(connection);
    }
    m_screenConnections.clear();

    for (QScreen *screen : QGuiApplication::screens()) {
        m_screenConnections.push_back(connect(
            screen,
            &QScreen::geometryChanged,
            this,
            [this](const QRect &) {
                scheduleReselect();
            }
        ));
        m_screenConnections.push_back(connect(
            screen,
            &QScreen::availableGeometryChanged,
            this,
            [this](const QRect &) {
                scheduleReselect();
            }
        ));
        m_screenConnections.push_back(connect(
            screen,
            &QScreen::logicalDotsPerInchChanged,
            this,
            [this](qreal) {
                scheduleReselect();
            }
        ));
        m_screenConnections.push_back(connect(
            screen,
            &QScreen::physicalDotsPerInchChanged,
            this,
            [this](qreal) {
                scheduleReselect();
            }
        ));
        m_screenConnections.push_back(connect(
            screen,
            &QScreen::orientationChanged,
            this,
            [this](Qt::ScreenOrientation) {
                scheduleReselect();
            }
        ));
    }
}

}  // namespace signage::display
