tix_module(index_mem
  SOURCES src/index_mem.c
  DEPS base index
  TESTS index_mem
  TEST_DEPS tix::index_contract tix::testkit)
