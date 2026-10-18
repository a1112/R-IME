R-IME settings window chrome

Scope
- Main.qml (Linux/common settings) and Windows.qml (Qt 6.11 Fluent settings) use the same visible titlebar and native move/resize entry points.
- This does not implement macOS InputMethodKit. The full settings executable is not currently macOS compatible: its existing IPC client requires Linux SOCK_CLOEXEC/SOCK_NONBLOCK/ucred/SO_PEERCRED. The appearance module and isolated QML can be compiled/tested separately on macOS without changing that IPC/security implementation.
- Fcitx/Wayland and Windows TSF candidate surfaces retain their input-method/no-activation roles; they are not ordinary document windows and are unchanged.
- Close still calls QWindow.close(): busy work prevents closing; unsaved edits open the existing discard confirmation. There is no new close-to-tray or unconditional quit path.
- Phrase/restore/discard Dialogs stay in-window; FileDialog stays system managed. Modal visibility disables title gestures, controls, shortcuts and custom resize handles. Text input is not covered by drag handlers.
- macOS retains the invisible titled/resizable/closable AppKit style bits, hides native title text/buttons and uses public NSVisualEffectView BehindWindow. Normal content radius is 10; maximized/fullscreen radius is 0. Windows attempts supported DWM Desktop Acrylic then Mica (wallpaper material), with solid fallback elsewhere.
- Titlebar double-click reads macOS AppleActionOnDoubleClick (Minimize, None or zoom/default) and delegates through public AppKit performMiniaturize/performZoom with real native capability checks. Fullscreen or attached-sheet state rejects the action; fullscreen/modal title dragging is also blocked. Explicit maximize/fullscreen buttons keep their behavior. Pure policy and real QML dispatch tests do not change system preferences.
- Native effects can be disabled from the titlebar. Reduce Transparency / increased contrast also force solid surfaces. The preference uses the settings application's QSettings profile; preview QA does not read or write it. No per-move/resize effects work or recurring preference polling is added.

Isolated compile and synthetic validation (no engine or IME installation)
  cmake -S . -B build-chrome -G Ninja -DRIME_BUILD_ENGINE=OFF -DRIME_BUILD_FCITX=OFF -DRIME_BUILD_WINDOWS_TSF=OFF -DRIME_BUILD_CONTROL=OFF -DRIME_BUILD_SETTINGS=ON -DBUILD_TESTING=ON
  cmake --build build-chrome --target settings-chrome-test window-placement-test -j2
  ctest --test-dir build-chrome -R '^(settings-chrome|window-placement)$' --output-on-failure

On a supported Linux/Windows toolchain, also compile the r-ime-settings target. The full macOS target currently fails at the unchanged Linux IPC source described above. Do not replace its credential checks to make an appearance test pass.

The chrome test forces offscreen/software rendering and temporary QSettings. It loads production QML with a synthetic settings object; it never constructs SettingsBackend, starts an engine, connects to sockets, registers an input method or sends actual system input. Synthetic QInputMethodEvent coverage checks the QML text field's preedit/commit path only. Geometry tests also use temporary profiles. The product binary is not launched by these commands.

Native acceptance still required
- Real title dragging/cancellation, eight-direction resizing, native fullscreen and screen-scale transitions.
- Keyboard/VoiceOver focus, pointer cancellation on controls, busy/dirty close and native FileDialog/modal behavior.
- macOS material pixels, accessibility preference changes, corners and shadow. Retained invisible style bits are not a removed AppKit window style.
- Windows Fluent/DWM and Linux compositor behavior on their actual platforms. Windows preference refresh occurs on creation/show/focus/theme, not a dedicated WM_SETTINGCHANGE observer.
- Offscreen results do not establish native frame timing, CPU use, drag latency or smoothness. Do not install or activate an IME merely to validate settings chrome.
