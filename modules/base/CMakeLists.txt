tix_module(base
  SOURCES src/base.c src/buf.c src/oid.c src/json.c src/date.c
  TESTS base
  TEST_DEPS tix::testkit)
