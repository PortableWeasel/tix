tix_module(store_mem
  SOURCES src/store_mem.c
  DEPS base store
  TESTS store_mem
  TEST_DEPS tix::testkit tix::store_contract)
