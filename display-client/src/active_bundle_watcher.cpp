#include "active_bundle_watcher.h"

#include "bundle_parser.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>

namespace signage::display {

ActiveBundleWatcher::ActiveBundleWatcher(
    QString contentRoot,
    QObject *parent
)
    : QObject(parent)
    , m_contentRoot(QDir::cleanPath(std::move(contentRoot)))
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(200);

    connect(
        &m_watcher,
        &QFileSystemWatcher::directoryChanged,
        this,
        [this](const QString &) {
            refreshWatchPaths();
            m_debounce.start();
        }
    );
    connect(&m_debounce, &QTimer::timeout, this, &ActiveBundleWatcher::reloadNow);

    refreshWatchPaths();
    QTimer::singleShot(0, this, &ActiveBundleWatcher::reloadNow);
}

QString ActiveBundleWatcher::contentRoot() const
{
    return m_contentRoot;
}

void ActiveBundleWatcher::reloadNow()
{
    refreshWatchPaths();
    const BundleParseResult result = BundleParser::loadActive(m_contentRoot);
    if (!result.isValid()) {
        emit loadFailed(result.error);
        return;
    }

    const BundleSnapshot &snapshot = *result.snapshot;
    if (snapshot.bundleId == m_lastEmittedBundleId) {
        return;
    }
    m_lastEmittedBundleId = snapshot.bundleId;
    emit snapshotReady(snapshot);
}

void ActiveBundleWatcher::refreshWatchPaths()
{
    const QString parentPath = QFileInfo(m_contentRoot).absolutePath();
    const QStringList candidates{parentPath, m_contentRoot};
    const QStringList watched = m_watcher.directories();
    for (const QString &candidate : candidates) {
        if (
            QFileInfo(candidate).isDir()
            && !watched.contains(candidate)
            && !m_watcher.addPath(candidate)
        ) {
            qWarning().noquote()
                << "Could not watch display content directory:"
                << candidate;
        }
    }
}

}  // namespace signage::display
