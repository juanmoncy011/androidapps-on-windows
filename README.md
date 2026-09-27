# Native Integration for Android Applications on Windows (Group 11)

A WSA-style demo split into five modules:

| # | Module | Folder | Status |
|---|--------|--------|--------|
| 1 | Launcher - scan folder, extract APK metadata, Win32 UI | `core/`, `launcher/`, `tools/` | done |
| 2 | Package Manager - install APKs into managed dir + SQLite | `pkgmgr/` | todo |
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

## Build (Windows)

Requires Visual Studio with the **Desktop development with C++** workload (includes CMake). Internet is needed on first configure to fetch miniz.

```bat
cmake -S . -B build
cmake --build build --config Release
build\bin\Release\AowLauncher.exe "C:\path\to\apks"
build\bin\Release\apkinfo.exe "C:\path\to\app.apk"
```

Shortcuts in the launcher: **F5** rescan, **Ctrl+O** open folder.
