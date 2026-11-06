# Text forms of field values, shared by frontends (CLI options, web forms).
tix_module(input
  SOURCES src/input.c
  DEPS base model core
  TESTS input
  TEST_DEPS tix::testkit tix::store_mem)
