#include "../src/settings/window_placement.hpp"
#include <QTemporaryDir>
#include <cassert>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName("R-IME-Window-Test");
    QCoreApplication::setApplicationName("Geometry");
    QTemporaryDir config;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
    QWindow fresh;
    installWindowPlacement(&fresh, true);
    const QRect work = fresh.screen()->availableGeometry();
    assert(fresh.width() <= int(work.width() * .8));
    assert(fresh.height() <= int(work.height() * .8));
    assert(work.contains(fresh.geometry()));
    assert(fresh.minimumWidth() <= fresh.width());
    assert(fresh.minimumHeight() <= fresh.height());
    assert(!QSettings().contains("window/normalGeometry"));
    // A user-sized, edge-aligned valid window must not get the fresh-start cap.
    const QRect saved = work.adjusted(2, 2, -2, -2);
    QSettings().setValue("window/normalGeometry", saved);
    QWindow restored;
    installWindowPlacement(&restored, false);
    assert(restored.geometry() == saved);
    // A missing monitor is corrected back into the current work area.
    QSettings().setValue("window/normalGeometry", QRect(-10000, -10000, 700, 500));
    QWindow unplugged;
    installWindowPlacement(&unplugged, false);
    assert(work.contains(unplugged.geometry()));
    return 0;
}
