#include "backend.hpp"
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QImage>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("R-IME Settings");
    QCoreApplication::setOrganizationName("R-IME");
    QGuiApplication::setApplicationDisplayName(QString::fromUtf8("R-IME 设置"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/branding/r-ime.svg")));
    QQuickStyle::setStyle("Basic");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(
        {"smoke-test", "Load the QML interface and quit without connecting to the engine."});
    parser.addOption(
        {"screenshot", "Save a settings screenshot without connecting to the engine.", "path"});
    parser.addOption({"page",
                      "Initial page: general, schemes, phrases, shortcuts, appearance, backup.",
                      "page", "general"});
    parser.addOption({"theme", "Preview theme: system, light, dark.", "theme", "system"});
    parser.process(app);
    const bool qa = parser.isSet("smoke-test") || parser.isSet("screenshot");
    SettingsBackend backend(rime_ime::Paths::from_environment(), !qa);
    QQmlApplicationEngine engine;
    bool qmlFailed = false;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [&](const QList<QQmlError> &) { qmlFailed = true; });
    engine.rootContext()->setContextProperty("settings", &backend);
    engine.rootContext()->setContextProperty("initialPage", parser.value("page"));
    engine.rootContext()->setContextProperty("previewTheme", parser.value("theme"));
    engine.load(QUrl("qrc:/settings/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (qa)
        QTimer::singleShot(900, &app, [&] {
            if (parser.isSet("screenshot")) {
                auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
                if (!window || !window->grabWindow().save(parser.value("screenshot")))
                    qmlFailed = true;
            }
            app.exit(qmlFailed ? 2 : 0);
        });
    return app.exec();
}
