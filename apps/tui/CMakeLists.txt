# tix-tui: the terminal UI (architecture §14.5), a composition root like apps/cli.
# curses is used only by src/term_curses.c (cmake/Quarantine.cmake).
pkg_check_modules(NCURSESW REQUIRED IMPORTED_TARGET ncursesw)
add_executable(tix-tui src/main.c src/tui.c src/term_curses.c)
target_link_libraries(tix-tui PRIVATE tix::core tix::input tix::index_sqlite tix::store_libgit tix_apps_common
  PkgConfig::NCURSESW)
tix_apply_warnings(tix-tui)
set_target_properties(tix-tui PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/cli)
install(TARGETS tix-tui RUNTIME DESTINATION bin)

# Driven in a pseudo-terminal through a terminal emulator (pexpect + pyte, run with uv).
find_program(TIX_UV_TUI uv)
if(TIX_UV_TUI)
  add_test(NAME tui.pty
    COMMAND ${TIX_UV_TUI} run --quiet --with pexpect --with pyte python -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/pty.py
      $<TARGET_FILE:tix> $<TARGET_FILE:tix-tui>)
  set_tests_properties(tui.pty PROPERTIES LABELS tui TIMEOUT 180 SKIP_RETURN_CODE 77)
endif()
