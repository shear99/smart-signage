#include "layout_geometry.h"

#include <algorithm>

namespace signage::display {

QRectF LayoutGeometry::safeRect(
    const QSizeF &windowSize,
    const EdgeInsets &insets
)
{
    const qreal width = std::max<qreal>(
        0.0,
        windowSize.width() * (1.0 - insets.left - insets.right)
    );
    const qreal height = std::max<qreal>(
        0.0,
        windowSize.height() * (1.0 - insets.top - insets.bottom)
    );
    return {
        windowSize.width() * insets.left,
        windowSize.height() * insets.top,
        width,
        height,
    };
}

QRectF LayoutGeometry::fitRect(
    const QRectF &container,
    const QSizeF &sourceSize
)
{
    if (
        container.width() <= 0.0 || container.height() <= 0.0
        || sourceSize.width() <= 0.0 || sourceSize.height() <= 0.0
    ) {
        return {};
    }

    const qreal scale = std::min(
        container.width() / sourceSize.width(),
        container.height() / sourceSize.height()
    );
    const QSizeF fitted = sourceSize * scale;
    return {
        container.center().x() - fitted.width() / 2.0,
        container.center().y() - fitted.height() / 2.0,
        fitted.width(),
        fitted.height(),
    };
}

QRectF LayoutGeometry::cropSourceRect(
    const QSizeF &containerSize,
    const QSizeF &sourceSize
)
{
    if (
        containerSize.width() <= 0.0 || containerSize.height() <= 0.0
        || sourceSize.width() <= 0.0 || sourceSize.height() <= 0.0
    ) {
        return {};
    }

    const qreal containerRatio =
        containerSize.width() / containerSize.height();
    const qreal sourceRatio = sourceSize.width() / sourceSize.height();

    if (sourceRatio > containerRatio) {
        const qreal croppedWidth = sourceSize.height() * containerRatio;
        return {
            (sourceSize.width() - croppedWidth) / 2.0,
            0.0,
            croppedWidth,
            sourceSize.height(),
        };
    }

    const qreal croppedHeight = sourceSize.width() / containerRatio;
    return {
        0.0,
        (sourceSize.height() - croppedHeight) / 2.0,
        sourceSize.width(),
        croppedHeight,
    };
}

}  // namespace signage::display
