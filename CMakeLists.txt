cmake_minimum_required(VERSION 3.28)
project(tix VERSION 0.0.1 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TIX_WERROR "Treat warnings as errors" OFF)
option(TIX_SANITIZE "Build with ASan + UBSan" OFF)

include(CTest)
include(cmake/TixModule.cmake)
include(cmake/GitLib.cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

# Modules, in dependency order (a module may only depend on ones above it).
add_subdirectory(modules/base)
add_subdirectory(modules/testkit)    # test support only
add_subdirectory(modules/store)
add_subdirectory(modules/store_contract)  # test support only
add_subdirectory(modules/store_mem)
add_subdirectory(modules/model)
add_subdirectory(modules/sim)        # test support only
add_subdirectory(modules/plan)
add_subdirectory(modules/export)
add_subdirectory(modules/index)
add_subdirectory(modules/index_contract)  # test support only
add_subdirectory(modules/index_mem)
add_subdirectory(modules/index_sqlite)
add_subdirectory(modules/core)
add_subdirectory(modules/input)
add_subdirectory(modules/store_libgit)

add_subdirectory(apps/cli)
