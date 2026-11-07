cmake_minimum_required(VERSION 3.28)
project(tix VERSION 0.7.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TIX_WERROR "Treat warnings as errors" OFF)
option(TIX_SANITIZE "Build with ASan + UBSan" OFF)

include(CTest)
include(cmake/TixModule.cmake)
include(cmake/GitLib.cmake)
include(cmake/Mongoose.cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
include(cmake/Quarantine.cmake)

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
add_subdirectory(modules/bridge)
add_subdirectory(modules/bridge_forgejo)

add_subdirectory(apps/common)
add_subdirectory(apps/cli)
add_subdirectory(apps/web)
add_subdirectory(apps/tui)

# contrib/night.py, the reference night runner (architecture §15.7): end to end with real tix
# clones and a fake agent. Needs uv (no packages).
find_program(TIX_UV uv)
if(TIX_UV)
  add_test(NAME contrib.night COMMAND ${TIX_UV} run --quiet python ${CMAKE_SOURCE_DIR}/contrib/tests/night_test.py
    $<TARGET_FILE:tix>)
  set_tests_properties(contrib.night PROPERTIES LABELS contrib TIMEOUT 300)
endif()
