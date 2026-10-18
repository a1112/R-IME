#pragma once
#ifdef __APPLE__
class QWindow;
class QObject;
struct MacOSAccessibilityPreferences {
    bool reduceTransparency = true;
    bool highContrast = false;
};
void configureMacOSNativeWindow(QWindow *window);
void performMacOSTitlebarDoubleClick(QWindow *window);
bool setMacOSBackdrop(QWindow *window, bool enabled);
void setMacOSCornerRadius(QWindow *window, int radius);
MacOSAccessibilityPreferences macOSAccessibilityPreferences();
void observeMacOSAccessibility(QWindow *window, QObject *receiver);
#endif
