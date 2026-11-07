# Renders a schedule as Mermaid, CSV, JSON or MS Project XML (pure; output goes to a tix_buf).
tix_module(export
  SOURCES src/export.c src/mspdi.c
  DEPS base model plan
  TESTS export
  TEST_DEPS tix::testkit)
