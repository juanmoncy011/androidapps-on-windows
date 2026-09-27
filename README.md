# Native Integration for Android Applications on Windows (Group 11)

A WSA-style demo split into five modules:

| # | Module | Folder | Status |
|---|--------|--------|--------|
| 1 | Launcher - scan folder, extract APK metadata, Win32 UI | `core/`, `launcher/`, `tools/` | done |
| 2 | Package Manager - install APKs into managed dir + SQLite | `pkgmgr/`, `tools/aowpm`, `tests/` | done |
| 3 | Runtime Manager - launch & track Android app processes | `runtime/`, `tools/aowrt` | done |
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

## Module 3 layout

- `runtime/` - **runtime** static library: the Runtime Manager service
  - `RuntimeManager::launch()` starts an installed app (one instance per app), `stop()` ends it, and a
    background monitor thread detects when apps exit or crash and raises events
  - every session is recorded in `runtime.db` (next to `packages.db`): package, backend, PID, start/end
    time, final state (running / exited / crashed / stopped / failed) and exit code. Because the state
    is shared, the launcher and the CLI see each other's apps, and PIDs are checked together with the
    process start time so a reused PID is never mistaken for the app
  - pluggable **runtime backends** (`createBackend`, chosen automatically or with `AOW_RUNTIME=adb|host`):
    - **adb** - a real Android runtime reached through `adb`: the Android Studio emulator, a phone over
      USB/Wi-Fi, BlueStacks, WSA... On launch the APK is deployed from the Package Manager store if the
      device does not have that version yet, then started with `am start`; the app's Android PID is
      tracked with `pidof`, and `stop` uses `am force-stop`
    - **host** - a simulated runtime for demos without Android: each app runs as a stand-in Windows
      process, `aowhost.exe`, whose window shows the app's icon, name and PID. It is launched, tracked
      and stopped exactly like a real app
- `tools/aowrt` - Runtime Manager CLI
- Launcher: **Launch** (Ctrl+L) and **Stop** buttons, "• Running" in the Status column, live status
  bar messages when an app starts, stops, exits or crashes (including apps started from `aowrt`)
- `tests/runtime_tests` - tests with real `aowhost` processes, plus `fake_adb`, a stand-in for `adb`,
  so the adb backend is tested without a device
- `common/` - `aowsql`, the SQLite wrapper shared by the Package Manager and the Runtime Manager

### Using a real Android runtime (optional)

Install [Android SDK platform-tools](https://developer.android.com/tools/releases/platform-tools) (or
Android Studio), start an emulator or connect a device, and check `adb devices` lists it. The launcher
and `aowrt` then pick the adb backend automatically (`aowrt backends` shows what was found). Set
`AOW_ADB` if `adb.exe` is not in the default SDK folder or on `PATH`, and `AOW_RUNTIME=host` to force
the simulated runtime.

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

build\bin\Release\aowrt.exe backends                    &:: which runtime will be used
build\bin\Release\aowrt.exe launch com.example.app       &:: add --wait to follow it until it exits
build\bin\Release\aowrt.exe ps
build\bin\Release\aowrt.exe watch                       &:: live start/exit/crash events
build\bin\Release\aowrt.exe stop com.example.app
build\bin\Release\aowrt.exe history

ctest --test-dir build -C Release --output-on-failure
```

Shortcuts in the launcher: **F5** rescan, **Ctrl+O** open folder, **Ctrl+I** install selected, **Ctrl+L** launch selected, **Ctrl+A** select all.
