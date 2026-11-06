# Spikes prove an approach; they are not modules and are deleted once the real
# module exists. M0 links git.git's libgit.a directly.
add_executable(tix_m0 m0/m0.c)
target_link_libraries(tix_m0 PRIVATE tix::gitlib)
# git's own source isn't -Wpedantic clean in macros; warnings only, never -Werror here.
target_compile_options(tix_m0 PRIVATE -Wall)
add_test(NAME spike.m0_roundtrip
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/m0/roundtrip.sh $<TARGET_FILE:tix_m0>)
set_tests_properties(spike.m0_roundtrip PROPERTIES LABELS spike TIMEOUT 60)
