#include "bundle_parser.h"

#include <QCoreApplication>
#include <QDebug>

using signage::display::BundleParseResult;
using signage::display::BundleParser;

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 2) {
        qCritical("usage: check-display-bundle <content-root>");
        return 2;
    }
    const BundleParseResult result = BundleParser::loadActive(
        application.arguments().at(1)
    );
    if (!result.isValid()) {
        qCritical().noquote() << result.error;
        return 1;
    }
    return 0;
}
