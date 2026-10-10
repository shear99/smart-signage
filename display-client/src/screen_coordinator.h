#pragma once

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTimer>

class QQuickWindow;

namespace signage::display {

class ScreenCoordinator final : public QObject {
    Q_OBJECT

public:
    explicit ScreenCoordinator(
        QQuickWindow *window,
        QString preferredEdidSha256,
        QString drmRoot,
        QObject *parent = nullptr
    );

    void start();

private slots:
    void scheduleReselect();
    void reselect();
    void reconnectScreenSignals();

private:
    QQuickWindow *m_window;
    QString m_preferredEdidSha256;
    QString m_drmRoot;
    QTimer m_debounce;
    QList<QMetaObject::Connection> m_screenConnections;
};

}  // namespace signage::display
