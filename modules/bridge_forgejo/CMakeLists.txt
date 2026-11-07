# The Forgejo tracker (architecture §14.4): the only module that includes curl.h.
tix_module(bridge_forgejo
  SOURCES src/forgejo.c
  DEPS base bridge
  EXTERNAL_PRIVATE PkgConfig::CURL PkgConfig::CJSON
  TEST_DEPS tix::testkit)

# Against a fake Forgejo (apps/../tests/fake_forgejo.py, run with uv): requests, auth,
# pagination, hostile replies. Skipped (77) without uv.
add_executable(test_bridge_forgejo tests/forgejo.c)
target_link_libraries(test_bridge_forgejo PRIVATE tix::bridge_forgejo tix::testkit)
tix_apply_warnings(test_bridge_forgejo)
find_program(TIX_UV_FJ uv)
if(TIX_UV_FJ)
  add_test(NAME bridge_forgejo.fake
    COMMAND ${TIX_UV_FJ} run --quiet python -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_forgejo.py
      $<TARGET_FILE:test_bridge_forgejo>)
  set_tests_properties(bridge_forgejo.fake PROPERTIES LABELS bridge TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
