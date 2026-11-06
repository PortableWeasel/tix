tix_module(model
  SOURCES src/model.c src/pack.c src/replay.c
  DEPS base
  EXTERNAL_PRIVATE PkgConfig::CJSON m
  TESTS pack replay convergence
  TEST_DEPS tix::testkit tix::sim tix::store_mem tix::store)
