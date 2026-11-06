tix_module(core
  SOURCES src/core.c src/sync.c
  DEPS base store model
  TESTS core
  TEST_DEPS tix::testkit tix::store_mem)
