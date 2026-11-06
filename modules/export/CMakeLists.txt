# Renders a schedule as Mermaid, CSV or JSON (pure; output goes to a tix_buf).
tix_module(export
  SOURCES src/export.c
  DEPS base model plan
  TESTS export
  TEST_DEPS tix::testkit)
