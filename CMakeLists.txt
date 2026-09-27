cmake_minimum_required(VERSION 3.20)
project(AndroidAppsOnWindows VERSION 0.1.0 LANGUAGES C CXX)

# Group 11 - Native Integration for Android Applications on Windows
# Modules: core (apkcore) -> launcher (M1) -> package manager (M2, pkgmgr/) -> runtime (M3) -> integration (M4) -> windowing (M5)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# Warnings/flags applied only to our own targets (not third-party code)
add_library(project_options INTERFACE)
if(MSVC)
  target_compile_options(project_options INTERFACE /W4 /utf-8 /permissive-)
else()
  target_compile_options(project_options INTERFACE -Wall -Wextra)
endif()

# ---- miniz (ZIP/deflate) -------------------------------------------------
# Set MINIZ_SOURCE_DIR to a folder containing miniz.c/miniz.h to build offline.
set(MINIZ_SOURCE_DIR "" CACHE PATH "Local miniz amalgamation (optional)")
if(MINIZ_SOURCE_DIR)
  set(miniz_SOURCE_DIR ${MINIZ_SOURCE_DIR})
else()
  include(FetchContent)
  FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip)
  FetchContent_MakeAvailable(miniz)
endif()

# Some miniz packagings expect a generated miniz_export.h; provide a no-op one.
file(WRITE ${CMAKE_BINARY_DIR}/miniz_gen/miniz_export.h "#pragma once\n#ifndef MINIZ_EXPORT\n#define MINIZ_EXPORT\n#endif\n")
add_library(miniz STATIC ${miniz_SOURCE_DIR}/miniz.c)
target_include_directories(miniz PUBLIC ${miniz_SOURCE_DIR} ${CMAKE_BINARY_DIR}/miniz_gen)
target_compile_definitions(miniz PRIVATE _CRT_SECURE_NO_WARNINGS)

# ---- SQLite (Package Manager database) ---------------------------------------
# Default: build the official amalgamation. Set SQLITE_SOURCE_DIR to a folder with
# sqlite3.c/sqlite3.h to build offline, or AOW_SYSTEM_SQLITE=ON to use an installed SQLite.
option(AOW_SYSTEM_SQLITE "Link the system SQLite instead of building the amalgamation" OFF)
set(SQLITE_SOURCE_DIR "" CACHE PATH "Local SQLite amalgamation (optional)")
if(AOW_SYSTEM_SQLITE)
  find_package(SQLite3 REQUIRED)
  add_library(sqlite3 INTERFACE)
  target_link_libraries(sqlite3 INTERFACE SQLite::SQLite3)
else()
  if(NOT SQLITE_SOURCE_DIR)
    include(FetchContent)
    FetchContent_Declare(sqlite
      URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip)
    FetchContent_MakeAvailable(sqlite)
    set(SQLITE_SOURCE_DIR ${sqlite_SOURCE_DIR})
  endif()
  add_library(sqlite3 STATIC ${SQLITE_SOURCE_DIR}/sqlite3.c)
  target_include_directories(sqlite3 PUBLIC ${SQLITE_SOURCE_DIR})
  target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_FOREIGN_KEYS=1)
  if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  endif()
endif()

# ---- modules ----------------------------------------------------------------
add_subdirectory(core)
add_subdirectory(pkgmgr)
add_subdirectory(tools)
if(WIN32)
  add_subdirectory(launcher)
endif()

option(AOW_BUILD_TESTS "Build unit tests" ON)
if(AOW_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
