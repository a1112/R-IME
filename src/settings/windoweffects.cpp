#include "windoweffects.h"
#include "effectspolicy.h"
#include "macosutils.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QSettings>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#endif

WindowEffects::WindowEffects(QObject *parent, bool preview) : QObject(parent), m_preview(preview)
{
    // QA is read/write isolated from the ordinary settings profile.
    if (!preview) m_enabled = QSettings().value(QStringLiteral("appearance/nativeEffects"), true).toBool();
    qApp->installEventFilter(this);
}

void WindowEffects::setWindow(QWindow *window)
{
    if (m_window == window) return;
    if (m_window) disconnect(m_window, nullptr, this, nullptr);
    m_window = window;
    if (window) {
        connect(window, &QWindow::windowStateChanged, this, &WindowEffects::refresh);
        connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
            if (visible) reconfigure(); // Also retry a handle unavailable before first show.
        });
    }
    reconfigure();
}

int WindowEffects::cornerRadius() const { return m_radius; }

void WindowEffects::titlebarDoubleClick()
{
#ifdef Q_OS_MACOS
    performMacOSTitlebarDoubleClick(m_window);
#endif
}

void WindowEffects::setEnabled(bool enabled)
{
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!m_preview) QSettings().setValue(QStringLiteral("appearance/nativeEffects"), enabled);
    refresh();
    emit changed();
}

void WindowEffects::reconfigure()
{
    m_cached = false;
#ifdef Q_OS_MACOS
    configureMacOSNativeWindow(m_window);
    observeMacOSAccessibility(m_window, this);
#endif
    refresh();
}

void WindowEffects::refresh()
{
    if (!m_window) return;
    bool reduced = true; // Unsupported platforms are opaque, never simulated blur.
    bool contrast = false;
#ifdef Q_OS_MACOS
    const auto preferences = macOSAccessibilityPreferences();
    reduced = preferences.reduceTransparency;
    contrast = preferences.highContrast;
#elif defined(Q_OS_WIN)
    HIGHCONTRASTW settings{sizeof(HIGHCONTRASTW)};
    contrast = !SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(settings), &settings, 0)
        || (settings.dwFlags & HCF_HIGHCONTRASTON);
    const QSettings personalize(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), QSettings::NativeFormat);
    reduced = !personalize.value(QStringLiteral("EnableTransparency"), 0).toBool();
#endif
    const bool allowed = WindowEffectsPolicy::materialAllowed(m_enabled, reduced, contrast);
    const int radius = WindowEffectsPolicy::cornerRadius(m_window->windowState() == Qt::WindowMaximized,
                                                        m_window->windowState() == Qt::WindowFullScreen);
    if (m_cached && m_allowed == allowed && m_highContrast == contrast && m_radius == radius) return;
    const bool materialChanged = !m_cached || m_allowed != allowed;
    m_cached = true;
    m_allowed = allowed;
    m_highContrast = contrast;
    m_radius = radius;
#ifdef Q_OS_MACOS
    if (materialChanged) m_active = setMacOSBackdrop(m_window, allowed);
    setMacOSCornerRadius(m_window, radius);
#elif defined(Q_OS_WIN)
    HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (materialChanged && hwnd) {
        int backdrop = allowed ? 3 : 1; // Desktop Acrylic, otherwise DWMSBT_NONE.
        HRESULT result = DwmSetWindowAttribute(hwnd, 38, &backdrop, sizeof(backdrop));
        if (allowed && FAILED(result)) {
            backdrop = 2; // Mica is wallpaper material, not live behind-window blur.
            result = DwmSetWindowAttribute(hwnd, 38, &backdrop, sizeof(backdrop));
        }
        m_active = allowed && SUCCEEDED(result);
        const int margin = m_active ? -1 : 0;
        MARGINS margins{margin, margin, margin, margin};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
    }
    const int corners = radius > 0 ? 2 : 1;
    if (hwnd) DwmSetWindowAttribute(hwnd, 33, &corners, sizeof(corners));
#else
    Q_UNUSED(materialChanged);
    m_active = false;
#endif
    emit changed();
}

bool WindowEffects::eventFilter(QObject *, QEvent *event)
{
    switch (event->type()) {
    case QEvent::ApplicationPaletteChange:
    case QEvent::ThemeChange:
    case QEvent::ApplicationActivate:
    case QEvent::FocusIn:
        refresh();
        break;
    default:
        break;
    }
    return false;
}
