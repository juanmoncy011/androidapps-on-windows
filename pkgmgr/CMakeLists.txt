# pkgmgr: Package Manager (M2) - installs APKs into a managed folder, metadata in SQLite
add_library(pkgmgr STATIC
  src/package_manager.cpp
  src/sha256.cpp
)
add_library(aow::pkgmgr ALIAS pkgmgr)
target_include_directories(pkgmgr PUBLIC include PRIVATE src)
target_link_libraries(pkgmgr PUBLIC apkcore PRIVATE aowsql project_options)
