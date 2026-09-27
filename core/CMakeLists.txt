# apkcore: APK inspection library (used by Launcher now, Package Manager next)
add_library(apkcore STATIC
  src/zip_reader.cpp
  src/string_pool.cpp
  src/axml.cpp
  src/arsc.cpp
  src/apk_parser.cpp
  src/apk_scanner.cpp
)
add_library(aow::apkcore ALIAS apkcore)
target_include_directories(apkcore PUBLIC include PRIVATE src)
target_link_libraries(apkcore PRIVATE miniz project_options)
