#pragma once

#include "bundle_types.h"

#include <QRectF>
#include <QSizeF>

namespace signage::display {

class LayoutGeometry {
public:
    [[nodiscard]] static QRectF safeRect(
        const QSizeF &windowSize,
        const EdgeInsets &insets
    );

    [[nodiscard]] static QRectF fitRect(
        const QRectF &container,
        const QSizeF &sourceSize
    );

    [[nodiscard]] static QRectF cropSourceRect(
        const QSizeF &containerSize,
        const QSizeF &sourceSize
    );
};

}  // namespace signage::display
