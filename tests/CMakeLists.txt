# Unit tests: build synthetic APKs on the fly, so no binary fixtures are needed.
add_executable(pkgmgr_tests pkgmgr_tests.cpp test_apk.cpp)  # sha256 comes from pkgmgr
target_include_directories(pkgmgr_tests PRIVATE ../pkgmgr/src)
target_link_libraries(pkgmgr_tests PRIVATE pkgmgr miniz project_options)
add_test(NAME pkgmgr_tests COMMAND pkgmgr_tests)

# fake_adb: emulates the adb commands the runtime's adb backend uses (no device needed)
add_executable(fake_adb fake_adb.cpp)
target_link_libraries(fake_adb PRIVATE apkcore project_options)
if(MINGW)
  target_link_options(fake_adb PRIVATE -municode)
endif()

add_executable(runtime_tests runtime_tests.cpp test_apk.cpp)
target_include_directories(runtime_tests PRIVATE ../runtime/src)
target_link_libraries(runtime_tests PRIVATE runtime miniz project_options)
target_compile_definitions(runtime_tests PRIVATE
  AOWHOST_EXE="$<TARGET_FILE:aowhost>" FAKE_ADB_EXE="$<TARGET_FILE:fake_adb>")
add_dependencies(runtime_tests aowhost fake_adb)
add_test(NAME runtime_tests COMMAND runtime_tests)
