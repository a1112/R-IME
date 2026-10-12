# Version management

## Current version-management units

- `app`: `0.2.0`, source `CMakeLists.txt`

`versioning.json` declares exact canonical fields and verified mirrors, rather
than another version authority. The Python 3.11+ standard-library tool preserves
existing field formatting and all unrelated dependency, SDK, schema, migration
and toolchain versions. Cargo workspace members keep inheritance; only owned
local Cargo.lock records (no registry source/checksum) are mirrored.

## Safe local workflow

```sh
python3 scripts/version_manager.py show
python3 scripts/version_manager.py check
python3 scripts/version_manager.py bump patch --unit UNIT
python3 scripts/version_manager.py bump patch --unit UNIT --expect CURRENT --apply
python3 scripts/version_manager.py set X.Y.Z-preview.1 --unit UNIT
python3 -m unittest discover -s tests/versioning -p 'test_*.py'
```

Replace UNIT/CURRENT with exact values from `show`. Without `--apply`, changes
are previews only. An applied update requires `--expect` and exactly one unit;
stale values, malformed/missing/duplicate bindings, drift, unsafe paths and
unsupported platform versions fail closed. Prerelease-to-stable promotion needs
explicit `--allow-stable`; ordinary adoption never promotes or changes a version.
For drift, review ownership first, restore the intended mirror deliberately, then
run `check`; the tool does not guess which mismatched value is right. A crash can
leave `.versioning.lock` or recovery backups: inspect them before retrying.

The tool never runs package-manager hooks, lifecycle scripts, dependency resolution,
Git, network calls, commits, tags, releases, signing or publishing. Review the
preview and diff, update Unreleased notes, then run the relevant native build and
application tests before any separately authorized release. Versioning tests are
metadata checks only and do not imply native packaging, UI, service or platform
acceptance. Independent units are not bumped together.

## Scope and history

No published application release was identified in the audited local tags/releases or verified central release mapping on 2026-10-12. Existing manifest versions are retained, not relabeled as a release.

Central artifact preview versions may differ from current development manifests;
central tags point to the artifact repository, not the application's source commit.
The authoritative existing native release workflow remains in place. Explicit
unmanaged paths and reasons are listed in `versioning.json`; copied/vendored SDKs,
examples, historical generated artifacts and protocol/database versions are not
updated by application bump commands.

A separately selectable version-management unit does not imply an independently published application. Package/crate authorities are kept separate unless an explicit workspace inheritance or owned application mirror binds them together. Publication evidence is listed separately above.
