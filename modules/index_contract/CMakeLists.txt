# Test support only: the shared tix_index contract.
tix_module(index_contract
  SOURCES src/index_contract.c
  DEPS base index testkit)
