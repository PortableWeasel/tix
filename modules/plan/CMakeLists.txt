# Pure scheduling engine (architecture §5): WBS, CPM, rollups, resource load.
tix_module(plan
  SOURCES src/plan.c
  DEPS base model
  TESTS plan
  TEST_DEPS tix::testkit)
