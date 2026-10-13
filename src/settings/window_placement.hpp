#pragma once
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QWindow>
#include <memory>

// QScreen work areas and QWindow geometry are already in logical pixels.
inline void installWindowPlacement(QWindow *window, bool preview) {
    auto settings = std::make_shared<QSettings>();
    auto fit = [window](QRect requested, bool fresh) {
        QScreen *screen = QGuiApplication::primaryScreen();
        int overlap = 0;
        for (auto *candidate : QGuiApplication::screens()) {
            const QRect intersection = requested.intersected(candidate->availableGeometry());
            const int area = intersection.width() * intersection.height();
            if (area > overlap) { overlap = area; screen = candidate; }
        }
        if (!screen) return;
        QRect work = screen->availableGeometry();
        const auto margins = window->frameMargins();
        const int edge = fresh ? 16 : 0;
        const int availableWidth = qMax(1, work.width() - edge * 2 - margins.left() - margins.right());
        const int availableHeight = qMax(1, work.height() - edge * 2 - margins.top() - margins.bottom());
        const int maxWidth = fresh ? qMin(availableWidth, int(work.width() * .8)) : availableWidth;
        const int maxHeight = fresh ? qMin(availableHeight, int(work.height() * .8)) : availableHeight;
        window->setMinimumSize(QSize(qMin(qMin(760, maxWidth), fresh ? 760 : requested.width()),
                                    qMin(qMin(520, maxHeight), fresh ? 520 : requested.height())));
        requested.setSize(QSize(qBound(window->minimumWidth(), requested.width(), maxWidth),
                                qBound(window->minimumHeight(), requested.height(), maxHeight)));
        const int left = work.left() + edge + margins.left();
        const int top = work.top() + edge + margins.top();
        const int right = work.x() + work.width() - edge - margins.right() - requested.width();
        const int bottom = work.y() + work.height() - edge - margins.bottom() - requested.height();
        if (fresh) requested.moveCenter(work.center());
        requested.moveLeft(qBound(left, requested.x(), qMax(left, right)));
        requested.moveTop(qBound(top, requested.y(), qMax(top, bottom)));
        window->setGeometry(requested);
    };
    const QRect saved = preview ? QRect() : settings->value("window/normalGeometry").toRect();
    fit(saved.isValid() ? saved : QRect(0, 0, 860, 620), !saved.isValid());
    window->show();
    if (preview) return; // Isolated QA never updates a user's window preference.
    auto timer = new QTimer(window);
    timer->setInterval(200);
    timer->setSingleShot(true);
    auto save = [window, settings] {
        if (window->visibility() == QWindow::Windowed) {
            settings->setValue("window/normalGeometry", window->geometry());
            settings->sync();
        }
    };
    QObject::connect(timer, &QTimer::timeout, window, save);
    auto schedule = [timer] { timer->start(); };
    QObject::connect(window, &QWindow::xChanged, window, schedule);
    QObject::connect(window, &QWindow::yChanged, window, schedule);
    QObject::connect(window, &QWindow::widthChanged, window, schedule);
    QObject::connect(window, &QWindow::heightChanged, window, schedule);
    QObject::connect(window, &QWindow::screenChanged, window, [window, fit] {
        if (window->visibility() == QWindow::Windowed) fit(window->geometry(), false);
    });
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, window, save);
}
