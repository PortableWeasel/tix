# tix — project rules

Distributed, git-native ticket tracker + project planner in C17. Design: `docs/architecture.md`
(v1.0 is the agreed baseline; change it deliberately, not by drift).

## Build / test

```bash
git submodule update --init --depth 1                       # third_party/git @ v2.47.3
cmake --preset dev && cmake --build --preset dev && ctest --preset dev   # Debug, -Werror, ASan+UBSan
cmake --preset release && cmake --build --preset release
```

All tests must pass under `dev` before committing.

## git.git (`libgit.a`)

- Pinned to **v2.47.3 = the system git**. Bump only together with the system git.
- Built in-tree by `cmake/GitLib.cmake` with git's Makefile (`prefix=/usr`,
  `gitexecdir=/usr/lib/git-core` so it reads the same config as `/usr/bin/git`). Feature
  macros are taken from git's own `ALL_CFLAGS`; never hand-edit them.
- **Only `modules/store_libgit/src/` (and `spikes/`) may include git.git headers or link
  `tix::gitlib`.** git headers: `#define USE_THE_REPOSITORY_VARIABLE`, then
  `git-compat-util.h` first.
- Programs linking libgit must define `common_exit()` (normally from git's common-main.c) and
  run: `sanitize_stdfds(); initialize_repository(the_repository); setup_git_directory();
  git_config(git_default_config, NULL);`.
- Network (fetch/push) is always a child `git` process (`child_process.git_cmd = 1`), argv-only.

## Module rules (enforced at configure time by cmake/TixModule.cmake)

- Module = `modules/<name>/` with `include/tix/<name>*.h` (public contract), `src/`, `tests/`,
  `CMakeLists.txt` calling `tix_module(...)`.
- Every `#include "tix/X.h"` must be the module itself or a declared `DEPS` (tests: + `TEST_DEPS`).
- Modules are added to the top-level CMakeLists in dependency order. No cycles by construction.
- Library modules never print, `exit()`, or read env/argv/clock; frontends do.
- Headers state REQUIRES / ENSURES / OWNS / BORROWS / RETURNS (loom conventions).

## Data safety

- Never force-push `refs/tix/*`; never rewrite op-pack history.
- Everything fetched from a remote is untrusted until `model` validates it.
