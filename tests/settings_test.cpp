#include "backend.hpp"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class SettingsTest : public QObject {
    Q_OBJECT
  private slots:
    void editsAreStagedAndDefaultsKeepPhrases() {
        QTemporaryDir dir;
        auto p = paths(dir);
        SettingsBackend b(p, false);
        QVERIFY(!b.dirty());
        QVERIFY(b.setValue("appearance", "page_size", 7));
        QCOMPARE(b.value("appearance", "page_size").toInt(), 7);
        QVERIFY(b.dirty());
        QVERIFY(!QFile::exists(QString::fromStdString(p.config.string())));
        QVERIFY(b.upsertPhrase(-1, "ceshi", QString::fromUtf8("测试"), 100));
        b.restoreDefaults();
        QCOMPARE(b.phrases().size(), 1);
        QCOMPARE(b.value("appearance", "page_size").toInt(), 5);
    }
    void invalidEditsLeavePreviousValues() {
        QTemporaryDir dir;
        SettingsBackend b(paths(dir), false);
        QVERIFY(!b.setValue("appearance", "page_size", 10));
        QCOMPARE(b.value("appearance", "page_size").toInt(), 5);
        QVERIFY(!b.error().isEmpty());
        QVERIFY(!b.setValue("shortcuts", "cancel", "Shift"));
        QCOMPARE(b.value("shortcuts", "cancel").toString(), QString("Escape"));
    }
    void phraseEditingRejectsDuplicatesAndSupportsRemoval() {
        QTemporaryDir dir;
        SettingsBackend b(paths(dir), false);
        QVERIFY(b.upsertPhrase(-1, "nihao", QString::fromUtf8("你好"), 200));
        QVERIFY(!b.upsertPhrase(-1, "nihao", QString::fromUtf8("你好"), 300));
        QCOMPARE(b.phrases().size(), 1);
        QVERIFY(!b.upsertPhrase(0, "INVALID", QString::fromUtf8("你好"), 300));
        QVERIFY(b.upsertPhrase(0, "nihao", QString::fromUtf8("您好"), 300));
        QCOMPARE(b.phrases()[0].toMap()["text"].toString(), QString::fromUtf8("您好"));
        b.removePhrase(0);
        QVERIFY(b.phrases().isEmpty());
    }
    void importIsAtomicAndExportRoundTrips() {
        QTemporaryDir dir;
        SettingsBackend b(paths(dir), false);
        auto file = dir.filePath("phrases.tsv");
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello\tworld\t123\nINVALID\tbad\t1\n");
        f.close();
        QVERIFY(!b.importPhrases(QUrl::fromLocalFile(file)));
        QVERIFY(b.phrases().isEmpty());
        QVERIFY(b.upsertPhrase(-1, "hello", "world", 123));
        QVERIFY(b.exportPhrases(QUrl::fromLocalFile(file)));
        SettingsBackend other(paths(dir), false);
        QVERIFY(other.importPhrases(QUrl::fromLocalFile(file)));
        QCOMPARE(other.phrases(), b.phrases());
    }
    void corruptConfigIsReportedAndNotReplaced() {
        QTemporaryDir dir;
        auto p = paths(dir);
        QFile f(QString::fromStdString(p.config.string()));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("broken");
        f.close();
        SettingsBackend b(p, false);
        QVERIFY(!b.error().isEmpty());
        b.restoreDefaults();
        QVERIFY(b.dirty());
        b.discard();
        QVERIFY(!b.dirty());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("broken"));
    }
    void discardRestoresBothSettingsAndPhrases() {
        QTemporaryDir dir;
        SettingsBackend b(paths(dir), false);
        QVERIFY(b.setValue("general", "learning", false));
        QVERIFY(b.upsertPhrase(-1, "hello", "world", 123));
        b.discard();
        QVERIFY(!b.dirty());
        QVERIFY(b.phrases().isEmpty());
        QVERIFY(b.value("general", "learning").toBool());
    }
    void failedApplyPreservesDraftAndDoesNotWriteConfig() {
        QTemporaryDir dir;
        auto p = paths(dir);
        SettingsBackend b(p, false);
        QVERIFY(b.setValue("appearance", "theme", "dark"));
        b.apply();
        QTRY_VERIFY_WITH_TIMEOUT(!b.busy(), 3000);
        QVERIFY(b.dirty());
        QVERIFY(!b.error().isEmpty());
        QVERIFY(!QFile::exists(QString::fromStdString(p.config.string())));
    }

  private:
    static rime_ime::Paths paths(const QTemporaryDir &dir) {
        auto root = std::filesystem::path(dir.path().toStdString());
        return {root / "config.json", root / "data", root / "cache", root / "runtime"};
    }
};
QTEST_GUILESS_MAIN(SettingsTest)
#include "settings_test.moc"
