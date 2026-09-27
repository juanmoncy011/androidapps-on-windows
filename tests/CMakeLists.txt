# Unit tests: build synthetic APKs on the fly, so no binary fixtures are needed.
add_executable(pkgmgr_tests pkgmgr_tests.cpp test_apk.cpp)  # sha256 comes from pkgmgr
target_include_directories(pkgmgr_tests PRIVATE ../pkgmgr/src)
target_link_libraries(pkgmgr_tests PRIVATE pkgmgr miniz project_options)
add_test(NAME pkgmgr_tests COMMAND pkgmgr_tests)
