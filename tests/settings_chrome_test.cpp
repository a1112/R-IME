// Offscreen production QML with synthetic settings only: no engine, sockets or IME registration.
#include "windoweffects.h"
#include "effectspolicy.h"
#include "rime_ime/config.hpp"
#include <QAccessible>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QPalette>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

class SyntheticSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap config MEMBER config NOTIFY changed)
    Q_PROPERTY(QVariantList phrases MEMBER phrases NOTIFY changed)
    Q_PROPERTY(bool dirty MEMBER dirty NOTIFY changed)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY stateChanged)
    Q_PROPERTY(QString error MEMBER error NOTIFY stateChanged)
    Q_PROPERTY(QString status MEMBER status NOTIFY stateChanged)
    Q_PROPERTY(QString diagnostic MEMBER diagnostic NOTIFY stateChanged)
public:
    QVariantMap config = QJsonDocument::fromJson(QByteArray::fromStdString(rime_ime::default_config().dump())).object().toVariantMap();
    QVariantList phrases;
    bool dirty = false, busy = false;
    QString error, status, diagnostic;
    int applyCalls = 0, fileCalls = 0;
    Q_INVOKABLE QVariant value(const QString &group, const QString &key) const { return config.value(group).toMap().value(key); }
    Q_INVOKABLE bool setValue(const QString &group, const QString &key, const QVariant &v) {
        if (busy) return false;
        auto map = config.value(group).toMap(); map[key] = v; config[group] = map;
        dirty = true; emit changed(); return true;
    }
    Q_INVOKABLE bool upsertPhrase(int, const QString &, const QString &, int) { dirty = true; emit changed(); return true; }
    Q_INVOKABLE void removePhrase(int) {}
    Q_INVOKABLE void discard() { dirty = false; emit changed(); }
    Q_INVOKABLE void apply() { ++applyCalls; }
    Q_INVOKABLE void restoreDefaults() {}
    Q_INVOKABLE bool importPhrases(const QUrl &) { ++fileCalls; return true; }
    Q_INVOKABLE bool exportPhrases(const QUrl &) { ++fileCalls; return true; }
    Q_INVOKABLE void importBackup(const QUrl &) { ++fileCalls; }
    Q_INVOKABLE void exportBackup(const QUrl &) { ++fileCalls; }
    Q_INVOKABLE void refreshStatus() {}
signals:
    void changed(); void stateChanged();
};
class SyntheticEffects : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled MEMBER enabled NOTIFY changed)
    Q_PROPERTY(bool active MEMBER active NOTIFY changed)
    Q_PROPERTY(bool highContrast MEMBER highContrast NOTIFY changed)
    Q_PROPERTY(int cornerRadius MEMBER radius NOTIFY changed)
public:
    bool enabled = true, active = false, highContrast = false;
    int radius = 10, titleDoubleClicks = 0;
    Q_INVOKABLE void setEnabled(bool value) { enabled = value; active &= enabled; emit changed(); }
    Q_INVOKABLE void titlebarDoubleClick() { ++titleDoubleClicks; }
signals:
    void changed();
};
class SyntheticTitleTarget : public QObject {
    Q_OBJECT
    Q_PROPERTY(int visibility MEMBER visibility NOTIFY changed)
    Q_PROPERTY(QString title MEMBER title CONSTANT)
public:
    int visibility = QWindow::Windowed;
    QString title = QStringLiteral("Synthetic title target");
    int moves = 0, maximizes = 0, restores = 0, minimizes = 0, fullscreens = 0;
    Q_INVOKABLE bool startSystemMove() { ++moves; return true; }
    Q_INVOKABLE void showMaximized() { ++maximizes; }
    Q_INVOKABLE void showNormal() { ++restores; }
    Q_INVOKABLE void showMinimized() { ++minimizes; }
    Q_INVOKABLE void showFullScreen() { ++fullscreens; }
    Q_INVOKABLE void close() {}
signals:
    void changed();
};
class Scene {
public:
    SyntheticSettings settings;
    SyntheticEffects effects;
    QQmlEngine engine;
    QQmlComponent component{&engine};
    std::unique_ptr<QQuickWindow> window;
    explicit Scene(const QString &file = QStringLiteral("Main.qml")) {
        engine.rootContext()->setContextProperty("settings", &settings);
        engine.rootContext()->setContextProperty("windowEffects", &effects);
        engine.rootContext()->setContextProperty("initialPage", QStringLiteral("general"));
        engine.rootContext()->setContextProperty("previewTheme", QStringLiteral("light"));
        component.loadUrl(QUrl::fromLocalFile(QStringLiteral(RIME_SETTINGS_QML_DIR) + "/" + file));
        window.reset(qobject_cast<QQuickWindow *>(component.create()));
        if (window) { window->setMinimumSize(QSize(1, 1)); window->show(); window->requestActivate(); QCoreApplication::processEvents(); }
    }
    QQuickItem *item(const char *name) { return window->findChild<QQuickItem *>(QString::fromLatin1(name)); }
    QObject *object(const char *name) { return window->findChild<QObject *>(QString::fromLatin1(name)); }
};
class CloseProbe : public QObject {
public:
    int requests = 0;
    bool eventFilter(QObject *, QEvent *event) override { if (event->type() == QEvent::Close) ++requests; return false; }
};
class SettingsChromeTest : public QObject {
    Q_OBJECT
    static QPoint center(QQuickItem *item) { return item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint(); }
    static void variants() { QTest::addColumn<QString>("file"); QTest::newRow("Main") << "Main.qml"; QTest::newRow("Windows-Fluent") << "Windows.qml"; }
private slots:
    void init() { QTest::failOnWarning(QRegularExpression(".*(Binding loop|Only binding to one of multiple|ReferenceError|TypeError).*")); }
    void policyAndPreviewIsolation() {
        for (bool enabled : {false,true}) for (bool reduced : {false,true}) for (bool contrast : {false,true})
            QCOMPARE(WindowEffectsPolicy::materialAllowed(enabled,reduced,contrast),enabled && !reduced && !contrast);
        QCOMPARE(WindowEffectsPolicy::cornerRadius(false,false),10);
        QCOMPARE(WindowEffectsPolicy::cornerRadius(true,false),0);
        QCOMPARE(WindowEffectsPolicy::cornerRadius(false,true),0);
        QSettings().setValue("appearance/nativeEffects",false);
        WindowEffects persisted;
        QVERIFY(!persisted.enabled());
        WindowEffects preview(nullptr,true);
        QVERIFY(preview.enabled());
        preview.setEnabled(false); preview.setEnabled(true);
        QCOMPARE(QSettings().value("appearance/nativeEffects").toBool(),false);
        persisted.setEnabled(true);
        WindowEffects reopened; QVERIFY(reopened.enabled());
    }
    void geometryDoesNotRebuildAndStateUpdatesCorners() {
        QQuickWindow window;
        WindowEffects effects(nullptr,true); effects.setWindow(&window);
        QSignalSpy changed(&effects,&WindowEffects::changed);
        for(int i=0;i<100;++i) {window.resize(500+i,400+i);window.setPosition(i,i);}
        QCoreApplication::processEvents(); QCOMPARE(changed.count(),0);
        window.setWindowState(Qt::WindowMaximized); QCOMPARE(effects.cornerRadius(),0);
        window.setWindowState(Qt::WindowFullScreen); QCOMPARE(effects.cornerRadius(),0);
        window.setWindowState(Qt::WindowNoState); QCOMPARE(effects.cornerRadius(),10);
        effects.setEnabled(false); QVERIFY(!effects.active());
    }
    void nativeTitleDoubleClickPolicy() {
        using Action = WindowEffectsPolicy::TitlebarAction;
        for (std::string_view preference : {"Minimize", "None", "Maximize", "Zoom", "", "unknown"})
            for (bool fullscreen : {false, true}) for (bool sheet : {false, true})
                for (bool minimize : {false, true}) for (bool zoom : {false, true}) {
                    Action expected = Action::None;
                    if (!fullscreen && !sheet && preference != "None") {
                        if (preference == "Minimize") { if (minimize) expected = Action::Minimize; }
                        else if (zoom) expected = Action::Zoom;
                    }
                    QCOMPARE(WindowEffectsPolicy::titlebarDoubleClickAction(preference,fullscreen,sheet,minimize,zoom),expected);
                }
        // Offscreen WIds are never cast to NSView; no native preference is read here.
        QQuickWindow window; WindowEffects effects(nullptr,true); effects.setWindow(&window);
        effects.titlebarDoubleClick(); QCOMPARE(window.visibility(),QWindow::Hidden);
    }
    void titleGesturesRespectFullscreenAndModal_data() { variants(); }
    void titleGesturesRespectFullscreenAndModal() {
        QFETCH(QString,file); SyntheticTitleTarget target; Scene s(file);
        QVERIFY2(s.window,qPrintable(s.component.errorString()));
        auto *chrome=s.item("settingsChrome"); auto *drag=s.item("windowDragRegion"); QVERIFY(chrome && drag);
        QVERIFY(chrome->setProperty("targetWindow",QVariant::fromValue(static_cast<QObject *>(&target))));
        target.visibility=QWindow::FullScreen; emit target.changed();
        QVERIFY(QMetaObject::invokeMethod(chrome,"titleDoubleClick"));
        QVERIFY(QMetaObject::invokeMethod(chrome,"startTitleDrag"));
        QCOMPARE(s.effects.titleDoubleClicks,0); QCOMPARE(target.moves,0); QCOMPARE(target.restores,0); QCOMPARE(target.maximizes,0);
        target.visibility=QWindow::Windowed; emit target.changed();
        QVERIFY(QMetaObject::invokeMethod(chrome,"titleDoubleClick"));
        QVERIFY(QMetaObject::invokeMethod(chrome,"startTitleDrag"));
#ifdef Q_OS_MACOS
        QCOMPARE(s.effects.titleDoubleClicks,1); QCOMPARE(target.maximizes,0);
#else
        QCOMPARE(s.effects.titleDoubleClicks,0); QCOMPARE(target.maximizes,1);
#endif
        QCOMPARE(target.moves,1);
        // mouseClick deliberately advances QtTest's clock beyond the double-click interval.
        QTest::mouseDClick(s.window.get(),Qt::LeftButton,Qt::NoModifier,center(drag));
#ifdef Q_OS_MACOS
        QTRY_COMPARE(s.effects.titleDoubleClicks,2);
#else
        QTRY_COMPARE(target.maximizes,2);
#endif
        auto *dialog=s.object("restoreDialog"); QVERIFY(QMetaObject::invokeMethod(dialog,"open"));
        QTRY_VERIFY(dialog->property("opened").toBool()); QVERIFY(!chrome->isEnabled());
        const int doubleClicks=s.effects.titleDoubleClicks, maximizes=target.maximizes;
        QVERIFY(QMetaObject::invokeMethod(chrome,"titleDoubleClick"));
        QVERIFY(QMetaObject::invokeMethod(chrome,"startTitleDrag"));
        QCOMPARE(s.effects.titleDoubleClicks,doubleClicks); QCOMPARE(target.moves,1); QCOMPARE(target.maximizes,maximizes);
        QVERIFY(QMetaObject::invokeMethod(dialog,"close")); QTRY_VERIFY(!dialog->property("visible").toBool());
        // Production DragHandler with a fake target; no native move loop is entered.
        const auto origin=center(drag);
        QTest::mousePress(s.window.get(),Qt::LeftButton,Qt::NoModifier,origin);
        QTest::mouseMove(s.window.get(),origin+QPoint(40,0),1);
        QTest::mouseMove(s.window.get(),origin+QPoint(60,0),1);
        QTest::mouseRelease(s.window.get(),Qt::LeftButton,Qt::NoModifier,origin+QPoint(60,0));
        QTRY_COMPARE(target.moves,2);
        QTest::mouseMove(s.window.get(),origin+QPoint(80,0),1); QCOMPARE(target.moves,2);
        target.visibility=QWindow::FullScreen; emit target.changed();
        QTest::mousePress(s.window.get(),Qt::LeftButton,Qt::NoModifier,origin);
        QTest::mouseMove(s.window.get(),origin+QPoint(40,0),1);
        QTest::mouseRelease(s.window.get(),Qt::LeftButton,Qt::NoModifier,origin+QPoint(40,0));
        QCOMPARE(target.moves,2); QCOMPARE(target.restores,0);
        QTest::mouseDClick(s.window.get(),Qt::LeftButton,Qt::NoModifier,origin);
        QCOMPARE(s.effects.titleDoubleClicks,doubleClicks); QCOMPARE(target.restores,0);
    }
    void controlsAndEdgesStaySeparate_data() {variants();}
    void controlsAndEdgesStaySeparate() {
        QFETCH(QString,file); Scene s(file); QVERIFY2(s.window,qPrintable(s.component.errorString()));
        auto *drag=s.item("windowDragRegion"); auto *handles=s.item("windowResizeHandles");auto *body=s.item("settingsBody");
        QVERIFY(drag && handles && body);
        for (const QSize size : {QSize(640,480),QSize(860,620),QSize(1280,900)}) {
            s.window->resize(size); QCoreApplication::processEvents();
            QCOMPARE(handles->size(),QSizeF(size)); QCOMPARE(handles->mapToScene(QPointF()),QPointF());
            QVERIFY(body->mapToScene(QPointF()).y()>=48);
            const QRectF dragRect(drag->mapToScene(QPointF()),drag->size());
            QList<QRectF> buttons;
            for(const char *name : {"windowEffectsButton","windowFullscreenButton","windowMinimizeButton","windowMaximizeButton","windowCloseButton"}) {
                auto *button=s.item(name); QVERIFY(button); QVERIFY(button->property("focusPolicy").toInt() & Qt::TabFocus);
                auto *accessible=QAccessible::queryAccessibleInterface(button); QVERIFY(accessible); QVERIFY(!accessible->text(QAccessible::Name).isEmpty());
                const QRectF rect(button->mapToScene(QPointF()),button->size()); QVERIFY(QRectF(QPointF(),size).contains(rect)); QVERIFY(!rect.intersects(dragRect)); buttons.append(rect);
            }
            int count=0;
            for(auto *handle:handles->childItems()) {
                if(!handle->property("modelData").isValid())continue;
                ++count; const QRectF rect(handle->mapToScene(QPointF()),handle->size()); QVERIFY(QRectF(QPointF(),size).contains(rect));
                for(const auto &button:buttons) QVERIFY(!rect.intersects(button));
            }
            QCOMPARE(count,8);
            for (const char *name : {"discardSettingsButton", "applySettingsButton"}) {
                auto *button = s.item(name); QVERIFY(button);
                const QRectF rect(button->mapToScene(QPointF()),button->size());
                QVERIFY2(QRectF(QPointF(),size).contains(rect),name);
            }
        }
    }
    void solidFallbackAndMaterial_data() {variants();}
    void solidFallbackAndMaterial() {
        QFETCH(QString,file); Scene s(file); QVERIFY2(s.window,qPrintable(s.component.errorString()));
        auto *surface=s.item("windowSurface"); QVERIFY(surface);
        QCOMPARE(surface->property("color").value<QColor>().alpha(),255);
        s.effects.active=true;emit s.effects.changed(); QTRY_VERIFY(surface->property("color").value<QColor>().alpha()<255);
        s.effects.highContrast=true; emit s.effects.changed(); QTRY_COMPARE(surface->property("color").value<QColor>(),QGuiApplication::palette().color(QPalette::Window));
        QCOMPARE(s.window->property("ink").value<QColor>(),QGuiApplication::palette().color(QPalette::WindowText));
        s.effects.radius=0;emit s.effects.changed();QTRY_COMPARE(surface->property("radius").toInt(),0);
        s.effects.highContrast=false;s.effects.active=false;s.effects.radius=10;emit s.effects.changed();QTRY_COMPARE(surface->property("color").value<QColor>().alpha(),255);
    }
    void ordinaryBusyDirtyCloseAndModalCancel_data(){variants();}
    void ordinaryBusyDirtyCloseAndModalCancel(){
        QFETCH(QString,file); Scene s(file); QVERIFY2(s.window,qPrintable(s.component.errorString()));
        auto *close=s.item("windowCloseButton");auto *dialog=s.object("closeDialog");auto *handles=s.item("windowResizeHandles");QVERIFY(close && dialog && handles);
        s.settings.busy=true;emit s.settings.stateChanged();QVERIFY(!close->isEnabled());QVERIFY(!s.window->close());QVERIFY(s.window->isVisible());
        s.settings.busy=false;s.settings.dirty=true;emit s.settings.stateChanged();emit s.settings.changed();
        close->forceActiveFocus();QTest::keyClick(s.window.get(),Qt::Key_Space);QTRY_VERIFY(dialog->property("opened").toBool());QVERIFY(s.window->isVisible());QVERIFY(!close->isEnabled());QVERIFY(!handles->isEnabled());
        QTest::keyClick(s.window.get(),Qt::Key_Escape);QTRY_VERIFY(!dialog->property("visible").toBool());QVERIFY(s.window->isVisible());QVERIFY(s.settings.dirty);
        QVERIFY(!s.window->close());QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog,"discarded"));QTRY_VERIFY(!s.window->isVisible());QVERIFY(s.window->property("allowClose").toBool());
    }
    void keyboardFullscreenCloseAndPointerCancel_data(){variants();}
    void keyboardFullscreenCloseAndPointerCancel(){
        QFETCH(QString,file);Scene s(file);QVERIFY2(s.window,qPrintable(s.component.errorString()));
        auto *close=s.item("windowCloseButton");auto *maximize=s.item("windowMaximizeButton");auto *drag=s.item("windowDragRegion");QVERIFY(close && maximize && drag);
        CloseProbe probe;s.window->installEventFilter(&probe);
        QTest::mousePress(s.window.get(),Qt::LeftButton,Qt::NoModifier,center(close));
        QTest::mouseMove(s.window.get(),center(drag));QTest::mouseRelease(s.window.get(),Qt::LeftButton,Qt::NoModifier,center(drag));
        QCOMPARE(probe.requests,0);QVERIFY(s.window->isVisible());
        QTest::keySequence(s.window.get(),QKeySequence(QKeySequence::FullScreen));QTRY_COMPARE(s.window->visibility(),QWindow::FullScreen);
        maximize->forceActiveFocus();QTest::keyClick(s.window.get(),Qt::Key_Space);QTRY_COMPARE(s.window->visibility(),QWindow::Windowed);
        QTest::keySequence(s.window.get(),QKeySequence(QKeySequence::Close));QTRY_VERIFY(!s.window->isVisible());QCOMPARE(probe.requests,1);
    }
    void restoreDialogCancelPreservesSettingsAndDisablesChrome_data(){variants();}
    void restoreDialogCancelPreservesSettingsAndDisablesChrome(){
        QFETCH(QString,file);Scene s(file);QVERIFY2(s.window,qPrintable(s.component.errorString()));
        auto *dialog=s.object("restoreDialog");auto *close=s.item("windowCloseButton");QVERIFY(dialog && close);
        QVERIFY(QMetaObject::invokeMethod(dialog,"open"));QTRY_VERIFY(dialog->property("opened").toBool());QVERIFY(!close->isEnabled());
        QTest::mouseClick(s.window.get(),Qt::LeftButton,Qt::NoModifier,center(close));QVERIFY(s.window->isVisible());QCOMPARE(s.settings.fileCalls,0);
        QTest::keyClick(s.window.get(),Qt::Key_Escape);QTRY_VERIFY(!dialog->property("visible").toBool());QCOMPARE(s.settings.fileCalls,0);QVERIFY(!s.settings.dirty);
        QVERIFY(s.object("filePicker")); // Native FileDialog is retained, never opened by this fixture.
    }
    void phraseInputKeepsPreeditAndModalKeyboard_data(){variants();}
    void phraseInputKeepsPreeditAndModalKeyboard(){
        QFETCH(QString,file);Scene s(file);QVERIFY2(s.window,qPrintable(s.component.errorString()));
        QVERIFY(QMetaObject::invokeMethod(s.window.get(),"editPhrase",Q_ARG(QVariant,QVariant(-1)),Q_ARG(QVariant,QVariant())));
        auto *dialog=s.object("phraseDialog");auto *field=s.item("phraseText");QVERIFY(dialog && field);QTRY_VERIFY(dialog->property("opened").toBool());field->forceActiveFocus();
        const QRect before=s.window->geometry();
        QInputMethodEvent preedit(QStringLiteral("nihao"),{});QCoreApplication::sendEvent(s.window.get(),&preedit);
        QTRY_COMPARE(field->property("preeditText").toString(),QStringLiteral("nihao"));
        QInputMethodEvent commit;commit.setCommitString(QString::fromUtf8("你好"));QCoreApplication::sendEvent(s.window.get(),&commit);
        QTRY_COMPARE(field->property("text").toString(),QString::fromUtf8("你好"));
        QTest::keySequence(s.window.get(),QKeySequence(QKeySequence::Close));QVERIFY(s.window->isVisible());QVERIFY(dialog->property("visible").toBool());
        QTest::keyClick(s.window.get(),Qt::Key_Escape);QTRY_VERIFY(!dialog->property("visible").toBool());QVERIFY(s.window->isVisible());QCOMPARE(s.window->geometry(),before);
        QCOMPARE(s.settings.applyCalls,0);QCOMPARE(s.settings.fileCalls,0);
    }
};
int main(int argc,char **argv){
    qputenv("QT_QPA_PLATFORM","offscreen");qputenv("QT_QUICK_BACKEND","software");
    QTemporaryDir profile;if(!profile.isValid())return 2;
    QGuiApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);QQuickStyle::setStyle("Basic");
    QCoreApplication::setOrganizationName("R-IME-Chrome-Test");QCoreApplication::setApplicationName("Isolated");
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,profile.path());
    SettingsChromeTest test;return QTest::qExec(&test,argc,argv);
}
#include "settings_chrome_test.moc"
