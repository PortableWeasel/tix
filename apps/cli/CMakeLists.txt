# The tix CLI: a composition root (may use any module). Installed also as git-tix,
# so `git tix ...` works.
add_executable(tix src/main.c src/cmd_id.c src/cmd_ticket.c src/cmd_sync.c src/editor.c)
target_link_libraries(tix PRIVATE tix::core tix::store_libgit)
tix_apply_warnings(tix)

add_custom_command(TARGET tix POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E create_symlink tix $<TARGET_FILE_DIR:tix>/git-tix
  VERBATIM)
install(TARGETS tix RUNTIME DESTINATION bin)
install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink tix \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/git-tix\")")

add_test(NAME cli.end_to_end COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.sh $<TARGET_FILE:tix>)
set_tests_properties(cli.end_to_end PROPERTIES LABELS cli TIMEOUT 120)
