#include "backend.hpp"
#include "client.hpp"
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

class SettingsEngineTest : public QObject {
    Q_OBJECT
  private slots:
    void applyBackupAndRestoreUseWorker() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto root = directory.path();
        const auto path = std::filesystem::path(root.toStdString());
        rime_ime::Paths paths{path / "config/r-ime/config.json", path / "data/r-ime",
                              path / "cache/r-ime", path / "runtime/r-ime"};
#ifdef _WIN32
        paths={path/"config/config.json",path/"data",path/"cache",path/"runtime"};
#endif
        std::filesystem::create_directories(path / "runtime");
        auto env = QProcessEnvironment::systemEnvironment();
#ifdef _WIN32
        env.insert("RIME_USER_ROOT",root);
#endif
        env.insert("HOME", root);
        env.insert("XDG_CONFIG_HOME", root + "/config");
        env.insert("XDG_DATA_HOME", root + "/data");
        env.insert("XDG_CACHE_HOME", root + "/cache");
        env.insert("XDG_RUNTIME_DIR", root + "/runtime");
        QProcess engine;
        engine.setProcessEnvironment(env);
        engine.setStandardOutputFile(QProcess::nullDevice());
        engine.setStandardErrorFile(root + "/engine.log");
        engine.start(QString::fromUtf8(RIME_SETTINGS_ENGINE_PATH),
                     {"--overlay", QString::fromUtf8(RIME_SETTINGS_OVERLAY_PATH)});
        QVERIFY(engine.waitForStarted());
        struct Guard {
            QProcess &engine;
            ~Guard() {
                engine.terminate();
                if (!engine.waitForFinished(5000)) {
                    engine.kill();
                    engine.waitForFinished();
                }
            }
        } guard{engine};
#ifdef _WIN32
        auto ready=[&]{try{rime_ime::Client client(paths.runtime/"engine.sock");return client.call({{"op","status"}},1000).value("ok",false);}catch(...){return false;}};
        QTRY_VERIFY_WITH_TIMEOUT(ready(),120000);
#else
        QTRY_VERIFY_WITH_TIMEOUT(std::filesystem::exists(paths.runtime / "engine.sock"), 120000);
#endif
        SettingsBackend b(paths, false);
        b.refreshStatus();
        QTRY_VERIFY_WITH_TIMEOUT(!b.busy(), 10000);
        QVERIFY2(b.error().isEmpty(), qPrintable(b.error()));
        QVERIFY(b.setValue("appearance", "theme", "dark"));
        QVERIFY(b.upsertPhrase(-1, "ceshi", QString::fromUtf8("设置测试"), 200));
        b.apply();
        QVERIFY(b.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!b.busy(), 180000);
        QVERIFY2(b.error().isEmpty(), qPrintable(b.error()));
        QVERIFY(!b.dirty());
        QCOMPARE(rime_ime::load_config(paths.config)["appearance"]["theme"].get<std::string>(),
                 std::string("dark"));
        auto file = QUrl::fromLocalFile(root + "/backup.json");
        b.exportBackup(file);
        QTRY_VERIFY_WITH_TIMEOUT(!b.busy(), 180000);
        QVERIFY2(b.error().isEmpty(), qPrintable(b.error()));
        QVERIFY(QFile::exists(file.toLocalFile()));
        QVERIFY(b.setValue("appearance", "theme", "light"));
        b.removePhrase(0);
        b.apply();
        QTRY_VERIFY_WITH_TIMEOUT(!b.busy(), 180000);
        QVERIFY2(b.error().isEmpty(), qPrintable(b.error()));
        b.importBackup(file);
        QTRY_VERIFY_WITH_TIMEOUT(!b.busy(), 180000);
        QVERIFY2(b.error().isEmpty(), qPrintable(b.error()));
        QCOMPARE(b.value("appearance", "theme").toString(), QString("dark"));
        QCOMPARE(b.phrases().size(), 1);
        QVERIFY(!b.dirty());
    }
};
QTEST_GUILESS_MAIN(SettingsEngineTest)
#include "settings_engine_test.moc"
