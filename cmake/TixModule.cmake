# tix_module(<name>
#   SOURCES  <src/...c>
#   DEPS     <tix modules this module's code includes directly>
#   EXTERNAL_PUBLIC  <imported targets exposed through the public header>
#   EXTERNAL_PRIVATE <imported targets used only by the implementation>
#   TESTS    <test names; tests/<name>.c each become a CTest>
#   TEST_DEPS <extra link targets for tests>)
#
# Builds static library tix_<name> (alias tix::<name>) with:
#   include/  -> PUBLIC  (the module's contract: include/tix/<name>*.h)
#   src/      -> PRIVATE (implementation detail, invisible to dependents)
#
# Boundary enforcement (configure-time): every `#include "tix/X.h"` in the
# module's sources, headers and tests must resolve to the module itself or a
# module listed in DEPS. Transitive includes do not count -- if you include
# it, declare it.

set_property(GLOBAL PROPERTY TIX_MODULES "")

function(_tix_header_owner header out)
  # tix/<stem>.h -> module <stem>, or <prefix> of <prefix>_<rest>.h
  get_property(known GLOBAL PROPERTY TIX_MODULES)
  string(REGEX REPLACE "\\.h$" "" stem "${header}")
  if(stem IN_LIST known)
    set(${out} "${stem}" PARENT_SCOPE)
    return()
  endif()
  string(REGEX MATCH "^[a-z0-9]+" prefix "${stem}")
  set(${out} "${prefix}" PARENT_SCOPE)
endfunction()

function(_tix_check_includes name deps test_deps)
  file(GLOB_RECURSE lib_files
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.[ch]
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
  file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.[ch])
  _tix_check_files("${name}" "${name};${deps}" "${lib_files}" "DEPS: ${deps}")
  _tix_check_files("${name}" "${name};${deps};${test_deps}" "${test_files}" "DEPS+TEST_DEPS: ${deps};${test_deps}")
endfunction()

function(_tix_check_files name allowed files declared)
  foreach(f IN LISTS files)
    file(STRINGS "${f}" lines REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]tix/")
    foreach(line IN LISTS lines)
      string(REGEX MATCH "tix/([A-Za-z0-9_]+\\.h)" _ "${line}")
      _tix_header_owner("${CMAKE_MATCH_1}" owner)
      if(NOT owner IN_LIST allowed)
        file(RELATIVE_PATH rel ${PROJECT_SOURCE_DIR} ${f})
        message(FATAL_ERROR
          "Module boundary violation in ${rel}:\n"
          "  includes tix/${CMAKE_MATCH_1} (module '${owner}')\n"
          "  but module '${name}' declares ${declared}")
      endif()
    endforeach()
  endforeach()
endfunction()

function(tix_apply_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
    -Wno-unused-parameter)
  target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
  if(TIX_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
  if(TIX_SANITIZE)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

function(tix_module name)
  cmake_parse_arguments(M "" "" "SOURCES;DEPS;EXTERNAL_PUBLIC;EXTERNAL_PRIVATE;TESTS;TEST_DEPS" ${ARGN})

  get_property(known GLOBAL PROPERTY TIX_MODULES)
  foreach(d IN LISTS M_DEPS)
    if(NOT d IN_LIST known)
      message(FATAL_ERROR "tix module '${name}' depends on unknown/later module '${d}'")
    endif()
  endforeach()

  add_library(tix_${name} STATIC ${M_SOURCES})
  add_library(tix::${name} ALIAS tix_${name})
  target_include_directories(tix_${name}
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  foreach(d IN LISTS M_DEPS)
    target_link_libraries(tix_${name} PUBLIC tix::${d})
  endforeach()
  target_link_libraries(tix_${name} PUBLIC ${M_EXTERNAL_PUBLIC} PRIVATE ${M_EXTERNAL_PRIVATE})
  tix_apply_warnings(tix_${name})

  set_property(GLOBAL APPEND PROPERTY TIX_MODULES ${name})
  set(test_mods "")
  foreach(t IN LISTS M_TEST_DEPS)
    if(t MATCHES "^tix::(.+)$")
      list(APPEND test_mods ${CMAKE_MATCH_1})
    endif()
  endforeach()
  _tix_check_includes(${name} "${M_DEPS}" "${test_mods}")

  foreach(t IN LISTS M_TESTS)
    set(exe test_${name}_${t})
    add_executable(${exe} tests/${t}.c)
    target_link_libraries(${exe} PRIVATE tix::${name} $<TARGET_NAME_IF_EXISTS:tix_testkit> ${M_TEST_DEPS})
    tix_apply_warnings(${exe})
    add_test(NAME ${name}.${t} COMMAND ${exe})
    set_tests_properties(${name}.${t} PROPERTIES LABELS "${name}" TIMEOUT 30)
  endforeach()
endfunction()
