# Windows TSF implementation plan

Goal: Implement the approved native Windows TSF adapter and install R-IME on this PC.

Architecture: Reuse the existing Rime engine and JSON configuration. A per-user named-pipe worker owns librime; the COM TSF DLL owns document composition and a no-activate candidate window. The existing Qt settings application connects to the same worker. Install protected binaries and both native COM views under Program Files/HKLM through an elevated helper; enable the profile, startup and settings for the current user. Preserve existing input methods and user data. Local verification showed that HKCU-only COM registration is insufficient for reliable TSF activation.

1. Port shared configuration filesystem operations, paths and IPC to Win32. Add native worker integration tests before implementation; validate real pinyin, double pinyin, composition, session ownership and restart.
2. Implement TSF COM activation, key sinks, edit sessions, composition cancellation, focus lifecycle, sensitive-input bypass and native candidate rendering. Add COM/TSF test harness. Register a distinct Chinese language profile, with explicit diagnostic and unregister commands.
3. Build x64 worker/settings and native TSF DLL, plus x86 TSF DLL for 32-bit applications. Use pinned official librime binaries and schema sources; stage a self-contained package with runtime libraries.
4. Test worker/config/settings and TSF host, install with a narrowly scoped elevated system phase, enumerate the registered/enabled profile, and exercise input in a real text control. Launch the worker from the ordinary user phase, preserve the previous default input method and provide uninstall.
5. Record actual machine results and any remaining application compatibility limits in docs/windows.md. No commits or remote publication are required.
