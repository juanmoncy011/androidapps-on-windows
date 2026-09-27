# Module 1 - Launcher: scans a folder for APKs and lists them in a Win32 UI;
# installs/uninstalls them through the Package Manager (M2)
add_executable(launcher WIN32
  src/main.cpp
  src/main_window.cpp
  src/main_window.h
  src/app.manifest
)
set_target_properties(launcher PROPERTIES OUTPUT_NAME "AowLauncher")
target_compile_definitions(launcher PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(launcher PRIVATE apkcore pkgmgr project_options
  comctl32 shell32 ole32 uxtheme windowscodecs)
if(MINGW)
  target_link_options(launcher PRIVATE -municode)
endif()
