# R-Box system adapter preparation

This branch prepares native registration commands for a protected, built-in
R-Box Windows adapter. It is not an accepted R-Box system installation package.

`r-ime-register --query-profile` reads Microsoft's TSF profile manager and emits
`exists`, `keyboardCategory`, and the current user's `enabled` state as JSON.
It does not instantiate R-IME's service CLSID or change registration. Enabled
state uses `IsEnabledLanguageProfile`; enumeration flags alone missed an actual
current-user override during native testing.

`--register-system <dll>` and `--unregister-system` omit the registration helper's
explicit current-user enable and obsolete HKCU COM-shadow cleanup. The original
`--register`/`--unregister` behavior is retained for existing installers. Microsoft
TSF's full behavior still needs verification in a clean native environment.
These commands do not establish ownership: R-Box must verify a protected owner
receipt, SID, package signature, all installed files and both registration views
before invoking any mutating command.

Read-only native verification:

```text
python tests/system_profile_query.py <x64-r-ime-register.exe>
python tests/system_profile_query.py <x86-r-ime-register.exe>
python tests/system_profile_query.py <R-Box-probe-rime.exe> --adapter-probe
```

The test snapshots the complete 32/64-bit machine/current-user COM and TIP
subtrees before and after querying. Both native helpers and the R-Box probe
passed on a machine containing an existing foreign R-IME installation; that
installation was preserved and R-Box refused takeover.

Pending: protected installation and ACL validation, same-user elevation,
durable system/database recovery, trusted settings and engine launch, clean
Windows input tests, update/uninstall, and the Linux system adapter.
