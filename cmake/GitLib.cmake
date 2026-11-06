# Builds git.git's internal libraries (libgit.a, xdiff/lib.a, reftable/libreftable.a)
# from the pinned submodule with git's own Makefile, and exposes them as the
# imported target tix::gitlib.
#
# Only modules/store_libgit (and spikes) may link tix::gitlib -- see
# docs/architecture.md section 2. git's headers are added with -isystem so our
# warning flags don't apply to them.
#
# git 2.47 builds in-tree; its objects are covered by git.git's .gitignore, so the
# submodule stays clean. The build is shared by all presets.

set(TIX_GIT_SRC ${PROJECT_SOURCE_DIR}/third_party/git)
if(NOT EXISTS ${TIX_GIT_SRC}/Makefile)
  message(FATAL_ERROR "third_party/git missing: run `git submodule update --init --depth 1`")
endif()

# prefix/gitexecdir match Debian's /usr/bin/git so libgit reads the same
# /etc/gitconfig and template dir as the system git it is pinned to.
set(TIX_GIT_MAKE_OPTS
  prefix=/usr gitexecdir=/usr/lib/git-core
  NO_GETTEXT=1 NO_TCLTK=1 NO_PERL=1 NO_PYTHON=1 NO_CURL=1 NO_EXPAT=1)

find_program(TIX_MAKE NAMES gmake make REQUIRED)

# Ask git's Makefile which feature macros it compiles with. Headers depend on
# them (e.g. the SHA backends change struct layouts), so we must match exactly.
set(_print_mk ${CMAKE_BINARY_DIR}/tix-git-print.mk)
file(WRITE ${_print_mk} "tix-print-cflags:\n\t@echo $(ALL_CFLAGS)\ntix-print-libs:\n\t@echo $(EXTLIBS)\n")
execute_process(
  COMMAND ${TIX_MAKE} -s -C ${TIX_GIT_SRC} ${TIX_GIT_MAKE_OPTS} -f Makefile -f ${_print_mk} tix-print-cflags
  OUTPUT_VARIABLE _git_cflags OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(
  COMMAND ${TIX_MAKE} -s -C ${TIX_GIT_SRC} ${TIX_GIT_MAKE_OPTS} -f Makefile -f ${_print_mk} tix-print-libs
  OUTPUT_VARIABLE _git_libs OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)

# Keep only plain feature macros (-DFOO, -DFOO=bar). String-valued ones are
# implementation-only (paths, CPU name) and lose their quoting through echo.
separate_arguments(_git_cflags UNIX_COMMAND "${_git_cflags}")
set(TIX_GIT_DEFINES "")
foreach(f IN LISTS _git_cflags)
  if(f MATCHES "^-D([^\"]+)$")
    list(APPEND TIX_GIT_DEFINES ${CMAKE_MATCH_1})
  endif()
endforeach()
separate_arguments(TIX_GIT_EXTLIBS UNIX_COMMAND "${_git_libs}")

set(_git_archives
  ${TIX_GIT_SRC}/libgit.a ${TIX_GIT_SRC}/xdiff/lib.a ${TIX_GIT_SRC}/reftable/libreftable.a)

add_custom_command(
  OUTPUT ${_git_archives}
  COMMAND ${TIX_MAKE} -C ${TIX_GIT_SRC} -j ${TIX_GIT_MAKE_OPTS} libgit.a xdiff/lib.a reftable/libreftable.a
  COMMENT "Building git.git internal libraries (v2.47.3)"
  USES_TERMINAL VERBATIM)
add_custom_target(tix_gitlib_build DEPENDS ${_git_archives})

add_library(tix_gitlib INTERFACE)
add_library(tix::gitlib ALIAS tix_gitlib)
add_dependencies(tix_gitlib tix_gitlib_build)
target_include_directories(tix_gitlib SYSTEM INTERFACE ${TIX_GIT_SRC})
target_compile_definitions(tix_gitlib INTERFACE ${TIX_GIT_DEFINES})
# libgit.a and its helper archives reference each other: link them as a rescan group.
target_link_libraries(tix_gitlib INTERFACE
  "$<LINK_GROUP:RESCAN,${TIX_GIT_SRC}/libgit.a,${TIX_GIT_SRC}/xdiff/lib.a,${TIX_GIT_SRC}/reftable/libreftable.a>"
  ${TIX_GIT_EXTLIBS})
