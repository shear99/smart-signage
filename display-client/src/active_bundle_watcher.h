#pragma once

#include "bundle_types.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>

namespace signage::display {

class ActiveBundleWatcher final : public QObject {
    Q_OBJECT

public:
    explicit ActiveBundleWatcher(
        QString contentRoot,
        QObject *parent = nullptr
    );

    [[nodiscard]] QString contentRoot() const;

public slots:
    void reloadNow();

signals:
    void snapshotReady(const signage::display::BundleSnapshot &snapshot);
    void loadFailed(const QString &error);

private:
    void refreshWatchPaths();

    QString m_contentRoot;
    QString m_lastEmittedBundleId;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};

}  // namespace signage::display
