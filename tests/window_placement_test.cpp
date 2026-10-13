#include "../src/settings/window_placement.hpp"
#include <QTemporaryDir>
#include <QtLogging>

#define CHECK(condition) do { if (!(condition)) qFatal("Window placement check failed: %s", #condition); } while (false)

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
    CHECK(fresh.width() <= int(work.width() * .8));
    CHECK(fresh.height() <= int(work.height() * .8));
    CHECK(work.contains(fresh.geometry()));
    CHECK(fresh.minimumWidth() <= fresh.width());
    CHECK(fresh.minimumHeight() <= fresh.height());
    CHECK(!QSettings().contains("window/normalGeometry"));
    // A user-sized, edge-aligned valid window must not get the fresh-start cap.
    const QRect saved = work.adjusted(2, 2, -2, -2);
    QSettings().setValue("window/normalGeometry", saved);
    QWindow restored;
    installWindowPlacement(&restored, false);
    CHECK(restored.geometry() == saved);
    // A size saved on a small screen stays small after moving to a larger screen.
    const QRect small(work.x() + 20, work.y() + 20, 500, 350);
    QSettings().setValue("window/normalGeometry", small);
    QWindow smallRestored;
    installWindowPlacement(&smallRestored, false);
    CHECK(smallRestored.geometry() == small);
    // A missing monitor is corrected back into the current work area.
    QSettings().setValue("window/normalGeometry", QRect(-10000, -10000, 700, 500));
    QWindow unplugged;
    installWindowPlacement(&unplugged, false);
    CHECK(work.contains(unplugged.geometry()));
    return 0;
}
