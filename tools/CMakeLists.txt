# apkinfo: command-line APK inspector for testing apkcore without the UI
add_executable(apkinfo apkinfo.cpp)
target_link_libraries(apkinfo PRIVATE apkcore project_options)
if(WIN32)
  target_compile_definitions(apkinfo PRIVATE NOMINMAX)
  if(MINGW)
    target_link_options(apkinfo PRIVATE -municode)
  endif()
endif()
