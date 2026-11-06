# The only module that may include sqlite3.h.
tix_module(index_sqlite
  SOURCES src/index_sqlite.c
  DEPS base index
  EXTERNAL_PRIVATE PkgConfig::SQLITE3
  TESTS index_sqlite
  TEST_DEPS tix::index_contract tix::testkit)
