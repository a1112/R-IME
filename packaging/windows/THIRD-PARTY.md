# Windows runtime and dictionary provenance

The package contains no network candidate service. These dependencies are bundled for offline use:

- librime 1.17.0, official MSVC x64 release `rime-33e7814-Windows-msvc-x64.7z`; SHA256 `7478c7caa4ff6b37de86daba1f7ce4a994a4f5ba24872a820fb2b3a9b01fed15`. Source: https://github.com/rime/librime/tree/33e7814 . BSD license included.
- Qt 6 runtime, dynamically linked from the selected official MSVC installation; LGPLv3/GPLv3/commercial licensing as applicable. The application can be rebuilt against a compatible Qt installation. Source: https://code.qt.io/cgit/qt/ .
- yaml-cpp 0.8.0 (MIT) and nlohmann/json 3.11.3 (MIT), built from pinned source archives.
- Microsoft VC143 redistributable runtime, copied from the installed Visual Studio redistribution directory.

`rime-data.tar.gz` contains unmodified text dictionaries/configuration and portable OpenCC dictionaries from Debian 13 packages. Precompiled Rime build caches are excluded. Upstream attribution and license notices are included in `licenses/`:

| Package | Version / upstream revision |
|---|---|
| rime-prelude | 0.0~git20250420.3c602fd-1 |
| rime-essay | 0.0~git20250407.573bbeb-1 |
| rime-data-luna-pinyin | 0.0~git20250412.781ba52-1 |
| rime-data-double-pinyin | 0.0~git20190120.69bf85d-5 |
| rime-data-stroke | 0.0~git20250118.7c9874c-1 |
| libopencc-data | 1.1.9+ds1-1+deb13u1 |

Rime upstream data repositories: https://github.com/rime ; OpenCC: https://github.com/BYVoid/OpenCC .
