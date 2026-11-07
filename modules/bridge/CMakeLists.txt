# Issue-tracker bridges (architecture §14.4): the tracker port and the sync algorithm.
tix_module(bridge
  SOURCES src/bridge.c
  DEPS base store model core
  TESTS bridge
  TEST_DEPS tix::testkit tix::store_mem)
