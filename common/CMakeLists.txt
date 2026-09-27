# aowsql: small RAII wrapper over SQLite shared by the Package Manager and Runtime Manager
add_library(aowsql STATIC src/sqlite_db.cpp)
target_include_directories(aowsql PUBLIC src)
target_link_libraries(aowsql PUBLIC sqlite3 PRIVATE project_options)
