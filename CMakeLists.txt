cmake_minimum_required(VERSION 3.20)
project(AndroidAppsOnWindows VERSION 0.1.0 LANGUAGES C CXX)

# Group 11 - Native Integration for Android Applications on Windows
# Modules: core (apkcore) -> launcher (M1) -> package manager (M2) -> runtime (M3) -> integration (M4) -> windowing (M5)

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

# ---- modules ----------------------------------------------------------------
add_subdirectory(core)
add_subdirectory(tools)
if(WIN32)
  add_subdirectory(launcher)
endif()
