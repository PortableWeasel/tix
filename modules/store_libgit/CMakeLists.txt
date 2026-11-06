# The only module that may include git.git headers or link tix::gitlib.
tix_module(store_libgit
  SOURCES src/store_libgit.c
  DEPS base store
  EXTERNAL_PRIVATE tix::gitlib
  TEST_DEPS tix::store_contract tix::testkit)
# git.git's headers are not -Wpedantic/-Wstrict-prototypes clean in macro use.
target_compile_options(tix_store_libgit PRIVATE -Wno-pedantic)

# Contract tests run once per repository format; libgit allows one repo per process.
add_executable(test_store_libgit_contract tests/contract.c)
target_link_libraries(test_store_libgit_contract PRIVATE tix::store_libgit tix::store_contract tix::testkit)
tix_apply_warnings(test_store_libgit_contract)
foreach(fmt sha1-files sha256-reftable)
  add_test(NAME store_libgit.contract_${fmt}
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-contract.sh $<TARGET_FILE:test_store_libgit_contract> ${fmt})
  set_tests_properties(store_libgit.contract_${fmt} PROPERTIES LABELS store_libgit TIMEOUT 60)
endforeach()
