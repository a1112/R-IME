#include "../src/settings/window_placement.hpp"
#include <QTemporaryDir>
#include <QEventLoop>
#include <QProcess>
#include <QtLogging>

#define CHECK(condition) do { if (!(condition)) qFatal("Window placement check failed: %s", #condition); } while (false)

static void checkLifecycle(QGuiApplication &app, const QString &state) {
    QCoreApplication::setApplicationName("Lifecycle-" + state);
    const QRect old(100, 100, 500, 400);
    { QSettings seed; seed.setValue("window/normalGeometry", old); seed.sync(); }
    QRect wanted;
    {
        QWindow window;
        installWindowPlacement(&window, false);
        QCoreApplication::processEvents();
        int xChanges = 0, yChanges = 0, widthChanges = 0, heightChanges = 0;
        QObject::connect(&window, &QWindow::xChanged, &window, [&] { ++xChanges; });
        QObject::connect(&window, &QWindow::yChanged, &window, [&] { ++yChanges; });
        QObject::connect(&window, &QWindow::widthChanged, &window, [&] { ++widthChanges; });
        QObject::connect(&window, &QWindow::heightChanged, &window, [&] { ++heightChanges; });
        window.setGeometry(QRect(120, 110, 600, 450));
        // Offscreen setGeometry queues native geometry notifications. Deliver
        // the normal resize first, without waiting for the 200ms save timer.
        QCoreApplication::processEvents();
        CHECK(xChanges > 0 && yChanges > 0 && widthChanges > 0 && heightChanges > 0);
        wanted = window.geometry();
        CHECK(QSettings().value("window/normalGeometry").toRect() == old);
        if (state == "maximized") window.showMaximized();
        else if (state == "minimized") window.showMinimized();
        else if (state == "fullscreen") window.showFullScreen();
        else if (state == "hidden") window.hide();
        else CHECK(window.close());
        // A state transition must persist the cached normal rectangle before
        // its window/timer can disappear, without waiting for application quit.
        CHECK(QSettings().value("window/normalGeometry").toRect() == wanted);
        if (state == "hidden") window.setGeometry(QRect(20, 20, 700, 500));
        QCoreApplication::processEvents();
        CHECK(QSettings().value("window/normalGeometry").toRect() == wanted);
        CHECK(QMetaObject::invokeMethod(&app, "aboutToQuit", Qt::DirectConnection));
        CHECK(QSettings().value("window/normalGeometry").toRect() == wanted);
        // A separate process cannot read the writer's in-memory QSettings cache.
        QProcess reader;
        reader.start(QCoreApplication::applicationFilePath(), {"--check-saved", QSettings().fileName(),
            QString::number(wanted.x()), QString::number(wanted.y()),
            QString::number(wanted.width()), QString::number(wanted.height())});
        CHECK(reader.waitForStarted(3000));
        CHECK(reader.waitForFinished(3000));
        CHECK(reader.exitStatus() == QProcess::NormalExit && reader.exitCode() == 0);
    }
    CHECK(QSettings().value("window/normalGeometry").toRect() == wanted);
    qInfo("Last normal geometry before %s: PASS", qPrintable(state));
}

static void checkDebounceAndPreview(QGuiApplication &app) {
    const QRect old(100, 100, 500, 400);
    QCoreApplication::setApplicationName("NormalDebounce");
    { QSettings seed; seed.setValue("window/normalGeometry", old); seed.sync(); }
    {
        QWindow window;
        installWindowPlacement(&window, false);
        QCoreApplication::processEvents();
        window.setGeometry(QRect(120, 110, 600, 450));
        QCoreApplication::processEvents();
        const QRect wanted = window.geometry();
        CHECK(QSettings().value("window/normalGeometry").toRect() == old);
        QEventLoop wait;
        QTimer::singleShot(250, &wait, &QEventLoop::quit);
        wait.exec();
        CHECK(QSettings().value("window/normalGeometry").toRect() == wanted);
    }
    qInfo("Normal window debounce: PASS");
    QCoreApplication::setApplicationName("PreviewIsolation");
    { QSettings seed; seed.setValue("window/normalGeometry", old); seed.sync(); }
    QWindow preview;
    installWindowPlacement(&preview, true);
    preview.setGeometry(QRect(120, 110, 600, 450));
    preview.hide();
    CHECK(QMetaObject::invokeMethod(&app, "aboutToQuit", Qt::DirectConnection));
    CHECK(QSettings().value("window/normalGeometry").toRect() == old);
    qInfo("Preview leaves existing preference unchanged: PASS");
}

static void checkScreenTransitions() {
    const QList<QRect> screens{QRect(0, 0, 1440, 900), QRect(1440, 0, 1440, 900)};
    CHECK(!windowGeometryNeedsRecovery(QRect(1050, 100, 800, 600), screens));
    CHECK(!windowGeometryNeedsRecovery(QRect(-300, 100, 800, 600),
                                      {QRect(-1440, 0, 1440, 900), screens.first()}));
    CHECK(!windowGeometryNeedsRecovery(QRect(1439, 100, 800, 600), {screens.first()}));
    CHECK(windowGeometryNeedsRecovery(QRect(1440, 100, 800, 600), {screens.first()}));
    CHECK(!windowGeometryNeedsRecovery(QRect(3000, 100, 800, 600), {}));
    qInfo("Screen intersection, negative origins and empty screen list: PASS");

    QCoreApplication::setApplicationName("ScreenTransitions");
    { QSettings seed; seed.setValue("window/normalGeometry", QRect(100, 100, 500, 400)); seed.sync(); }
    QWindow window;
    installWindowPlacement(&window, false);
    QCoreApplication::processEvents();
    auto *screen = window.screen();
    const QRect area = screen->geometry();
    window.setGeometry(QRect(area.right() - 150, area.top() + 100, 500, 400));
    QCoreApplication::processEvents();
    const QRect crossing = window.geometry();
    // Dispatch the actual production connection; this is not a native drag.
    CHECK(QMetaObject::invokeMethod(&window, "screenChanged", Qt::DirectConnection, Q_ARG(QScreen *, screen)));
    CHECK(window.geometry() == crossing);
    window.setGeometry(QRect(area.right() + 500, area.top() + 100, 500, 400));
    QCoreApplication::processEvents();
    CHECK(QMetaObject::invokeMethod(&window, "screenChanged", Qt::DirectConnection, Q_ARG(QScreen *, screen)));
    CHECK(screen->availableGeometry().contains(window.geometry()));
    qInfo("Production screenChanged connection preserves visible and recovers offscreen windows: PASS");
}

int main(int argc, char **argv) {
    if (argc == 7 && QString::fromLocal8Bit(argv[1]) == "--check-saved") {
        QCoreApplication app(argc, argv);
        QSettings stored(QString::fromLocal8Bit(argv[2]), QSettings::IniFormat);
        const QRect wanted(QString::fromLocal8Bit(argv[3]).toInt(), QString::fromLocal8Bit(argv[4]).toInt(),
                           QString::fromLocal8Bit(argv[5]).toInt(), QString::fromLocal8Bit(argv[6]).toInt());
        CHECK(stored.value("window/normalGeometry").toRect() == wanted);
        return 0;
    }
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("R-IME-Window-Test");
    QCoreApplication::setApplicationName("Geometry");
    QTemporaryDir config;
    CHECK(config.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
    {
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
        const QRect small(work.x() + 20, work.y() + 20,
                          qMax(1, qMin(500, work.width() - 40)),
                          qMax(1, qMin(350, work.height() - 40)));
        QSettings().setValue("window/normalGeometry", small);
        QWindow smallRestored;
        installWindowPlacement(&smallRestored, false);
        CHECK(smallRestored.geometry() == small);
        // A missing monitor is corrected back into the current work area.
        QSettings().setValue("window/normalGeometry", QRect(-10000, -10000, 700, 500));
        QWindow unplugged;
        installWindowPlacement(&unplugged, false);
        CHECK(work.contains(unplugged.geometry()));
    }
    qInfo("Startup, small saved geometry and missing monitor recovery: PASS");
    for (const QString state : {"maximized", "minimized", "fullscreen", "hidden", "closed"}) {
        checkLifecycle(app, state);
    }
    checkDebounceAndPreview(app);
    checkScreenTransitions();
    return 0;
}
