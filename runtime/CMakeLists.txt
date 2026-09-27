# runtime: Runtime Manager (M3) - launches installed apps and tracks their processes
add_library(runtime STATIC
  src/runtime_manager.cpp
  src/backends.cpp
  src/subprocess.cpp
)
add_library(aow::runtime ALIAS runtime)
target_include_directories(runtime PUBLIC include PRIVATE src)
target_link_libraries(runtime PUBLIC pkgmgr PRIVATE aowsql project_options)
if(WIN32)
  target_compile_definitions(runtime PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
endif()

# aowhost: stand-in app process for the simulated ("host") runtime backend
if(WIN32)
  add_executable(aowhost WIN32 host/aowhost.cpp)
  target_compile_definitions(aowhost PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
  target_link_libraries(aowhost PRIVATE project_options ole32 windowscodecs msimg32 shell32)
  if(MINGW)
    target_link_options(aowhost PRIVATE -municode)
  endif()
else()
  add_executable(aowhost host/aowhost.cpp)
  target_link_libraries(aowhost PRIVATE project_options)
endif()
