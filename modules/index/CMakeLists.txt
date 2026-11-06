# The cache port (vtable + inline wrappers). Adapters: index_sqlite, index_mem.
tix_module(index
  SOURCES src/index.c
  DEPS base model)
