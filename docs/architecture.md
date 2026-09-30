# R-IME architecture

Linux and Windows system input method, with a private macOS-inspired candidate panel.

The Fcitx adapter owns input focus and platform windows. A separate `r-ime-engine`
process owns librime, so other Fcitx Rime addons cannot share its global state.
The settings application is an independent Qt Quick process. All three components
share a versioned configuration and wire format; the core has no UI dependency.

## Boundaries

- Core: configuration validation, private platform paths, phrase import/export, backup.
- Engine: real librime sessions, newline-delimited JSON on Linux or length-framed JSON on Windows.
- Fcitx: key dispatch, client preedit, commit, status actions and custom panel callback.
- Candidate renderer: Cairo/Pango with Fcitx Wayland input-panel surfaces or X11 windows.
- Settings: six pages, live appearance preview, staged apply and deployment feedback.
- Windows: x64/x86 COM TSF adapters, synchronous edit sessions for consumed keys,
  no-activate GDI candidate panels, and a same-user named-pipe worker.

Wire requests carry `version`, `id`, `session` and `op`. Replies echo identifiers;
expired sessions cannot submit text to a newly focused application. Errors are data,
not text commits. User data lives only under the `r-ime` XDG namespace on Linux or
`%LOCALAPPDATA%\R-IME` on Windows. No network request is part of typing or learning.
Windows protected binaries and both COM architecture views are installed through
an elevated helper; the worker and settings run as the ordinary user.
macOS InputMethodKit remains a subsequent release.

## Acceptance

Build and automated integration tests run in Linux and Windows. Real KDE/Wayland testing is
required for popup positioning, focus, fractional scale and application coverage.
See `docs/verification.md` and `docs/windows.md` for actual results, not just planned checks.
