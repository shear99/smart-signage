#pragma once

#include "bundle_types.h"

#include <QString>

#include <optional>

namespace signage::display {

struct BundleParseResult {
    std::optional<BundleSnapshot> snapshot;
    QString error;

    [[nodiscard]] bool isValid() const
    {
        return snapshot.has_value();
    }
};

class BundleParser {
public:
    [[nodiscard]] static BundleParseResult loadActive(
        const QString &contentRoot
    );
};

}  // namespace signage::display
