# Native Integration for Android Applications on Windows (Group 11)

A WSA-style demo split into five modules:

| # | Module | Folder | Status |
|---|--------|--------|--------|
| 1 | Launcher - scan folder, extract APK metadata, Win32 UI | `core/`, `launcher/`, `tools/` | done |
| 2 | Package Manager - install APKs into managed dir + SQLite | `pkgmgr/`, `tools/aowpm`, `tests/` | done |
| 3 | Runtime Manager - launch & track Android app processes | `runtime/` | todo |
| 4 | Integration - clipboard sync Android <-> Windows | `integration/` | todo |
| 5 | Window Management - host Android windows as Win32 windows | `windowing/` | todo |

## Module 1 layout

- `core/` - **apkcore** static library (reused by later modules)
  - `zip_reader` - reads APKs as ZIP (miniz)
  - `axml` - parses binary `AndroidManifest.xml`
  - `arsc` - resolves `@string/app_name` and icon paths from `resources.arsc`
  - `apk_parser` / `apk_scanner` - produce `ApkInfo` (name, package, version, SDKs, ABIs, permissions, launch activity, icon)
- `launcher/` - Win32 app (`AowLauncher.exe`): folder picker, drag & drop, sortable list with icons, details on double-click, background scanning
- `tools/apkinfo` - CLI for testing the parser

## Module 2 layout

- `pkgmgr/` - **pkgmgr** static library (used by the Launcher now, Runtime Manager next)
  - `PackageManager::install()` validates the APK (parsable, not a split APK, safe package name),
    copies it into the managed store while computing its SHA-256, and records the metadata in SQLite
  - handles new installs, updates, same-version reinstalls, identical files (no-op) and refuses
    downgrades unless `allowDowngrade` is set - the same rules as `adb install`
  - `uninstall()`, `find()`, `list()`, `verify()` (detects missing/modified APKs and orphaned folders)
  - installs are atomic: the APK is staged next to its final path and renamed into place inside the
    database transaction, so the database never points at a half-copied file
  - thread-safe; several processes can share one store (SQLite WAL + busy timeout)
- Store layout (default `%LOCALAPPDATA%\AndroidAppsOnWindows`, override with `AOW_HOME`):

  ```
  packages.db                 packages, permissions, native_abis tables
  apps/<package>/base.apk     managed copy of the APK
  apps/<package>/icon.png     launcher icon extracted at install time
  ```
- `tools/aowpm` - Package Manager CLI
- Launcher: **Status** column (Not installed / Installed / Update available / Older), **Install** (Ctrl+I)
  and **Uninstall** buttons, multi-select (Ctrl+A); installs run in the background
- `tests/pkgmgr_tests` - unit tests; they generate real APKs on the fly, so no binary fixtures are needed

## Build (Windows)

Requires Visual Studio with the **Desktop development with C++** workload (includes CMake). Internet is needed on first configure to fetch miniz and the SQLite amalgamation (or pass `-DMINIZ_SOURCE_DIR=...` / `-DSQLITE_SOURCE_DIR=...` to build offline; on Linux `-DAOW_SYSTEM_SQLITE=ON` uses the installed SQLite).

```bat
cmake -S . -B build
cmake --build build --config Release
build\bin\Release\AowLauncher.exe "C:\path\to\apks"
build\bin\Release\apkinfo.exe "C:\path\to\app.apk"

build\bin\Release\aowpm.exe install "C:\path\to\apks"     &:: file or folder; add -r for subfolders, --downgrade to allow older versions
build\bin\Release\aowpm.exe list
build\bin\Release\aowpm.exe info com.example.app
build\bin\Release\aowpm.exe uninstall com.example.app
build\bin\Release\aowpm.exe verify

ctest --test-dir build -C Release --output-on-failure
```

Shortcuts in the launcher: **F5** rescan, **Ctrl+O** open folder, **Ctrl+I** install selected, **Ctrl+A** select all.
