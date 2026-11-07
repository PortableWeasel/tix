tix_module(core
  SOURCES src/core.c src/sync.c src/planning.c src/summary.c
  DEPS base store model plan index
  TESTS core
  TEST_DEPS tix::testkit tix::store_mem tix::index_mem)
