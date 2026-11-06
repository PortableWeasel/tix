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

# Modules, in dependency order (a module may only depend on ones above it).
# (none yet -- M1 adds base, model, store, store_mem)

add_subdirectory(spikes)
