# AGENTS.md — tix

Instructions for coding agents (and humans) working in this repository.

tix is a distributed, git-native ticket tracker and project planner written in C17. All
data (identities, tickets, resources, calendars) lives inside the git repository in
`refs/tix/*` as DAGs of commits carrying NDJSON op-packs, and syncs with plain
`git fetch`/`git push`. It links git.git's own `libgit.a`.

**The design is fixed in [`docs/architecture.md`](docs/architecture.md) (v1.0 baseline).**
Read §2 (layers), §3 (storage) and the section for whatever you are touching before you
change code. If an implementation needs to deviate from the doc, update the doc in the same
change and say why. Never let the code and the doc drift apart silently.

## Status

| Milestone | State |
|---|---|
| M0: link `libgit.a`, CAS refs, child-git sync | ✅ done (spike removed in M3) |
| M1: `base`, `model` (entity framework, identity, ticket tracking), `store` port, `store_mem`, convergence tests | ✅ done |
| M2: `store_libgit`, `core`, CLI basics | ✅ done |
| M3: pull/merge/push | ✅ done |
| M4: planning (WBS, scheduling, resources, calendars, `plan`, `export`) | ✅ done |
| M5: SQLite `index` | ✅ done |
| Later: signature enforcement, baselines, TUI, issue bridges (§11) | |

Work on the current milestone unless asked otherwise. Don't build ahead into later
milestones "while you're there".

## Layout

```
CMakeLists.txt          top level: modules are added here in dependency order
CMakePresets.json       dev (Debug, -Werror, ASan+UBSan) / release
cmake/GitLib.cmake      builds git.git's libgit.a, xdiff, reftable → tix::gitlib
cmake/TixModule.cmake   tix_module() + configure-time include-boundary enforcement
modules/<name>/         libraries: include/tix/<name>*.h, src/, tests/
                        base, store, store_mem, model, plan, export, index,
                        index_mem, index_sqlite, core, store_libgit;
                        testkit, sim, store_contract, index_contract are test-only
apps/cli/               the `tix` / `git-tix` frontend; tests/cli.sh is the end-to-end test
spikes/                 throwaway proofs, deleted once the real module exists (none now)
third_party/git/        git.git submodule, pinned at v2.47.3 (do not edit)
docs/architecture.md    the design
```

## Build and test

```bash
git submodule update --init --depth 1        # first time only
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset release && cmake --build --preset release
```

- **Every change must build warning-free and pass `ctest --preset dev`** (ASan+UBSan)
  before you commit. Report failures honestly, with the output. Don't weaken a test to
  make it pass.
- The first build compiles git.git (about 35 s). Later builds reuse it.
- Requirements: CMake ≥ 3.28, Ninja, GCC 14 / Clang, GNU make, zlib, cJSON (`libcjson-dev`) and SQLite 3 (`libsqlite3-dev`) headers.
- Use `uv run` for any Python helper. Never call the system Python directly.

## Architecture rules (hard constraints)

1. **Ports and adapters.** `model` and `plan` are pure: no I/O, no git, no clock, no env.
   Anything touching git, disk, processes or terminals is an adapter behind a vtable
   interface (`tix_store`, `tix_index`).
2. **Module boundaries are enforced by CMake.** A file may `#include "tix/X.h"` only for its
   own module or a module listed in `DEPS` (tests: plus `TEST_DEPS`). Transitive includes
   don't count, so declare what you include. Add modules to the top-level `CMakeLists.txt`
   in dependency order. Cycles are impossible by construction, so keep it that way.
3. **SQLite is quarantined** the same way: only `modules/index_sqlite/src/` includes
   `sqlite3.h`. The cache is disposable. Nothing may depend on its contents surviving.
4. **git.git is quarantined.** Only `modules/store_libgit/src/` (and `spikes/`) may include
   git.git headers or link `tix::gitlib`. No git types (`struct object_id`, `strbuf`, …)
   may appear in any `tix/*.h` public header. Convert at the adapter boundary.
5. **Frontends hold no business logic.** `apps/*` parse arguments, call `core`/`export`,
   and format output. If a CLI command needs a rule, the rule belongs in `core`, `model`
   or `plan`.
6. **Library modules never** print, call `exit()`/`abort()` for recoverable errors, read
   argv/env, or read the clock. "Now" and the local identity are passed in by the caller.
7. **Single-threaded** in v1 (git.git's library code is not thread-safe).

## C conventions

- C17, `-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes`,
  `-Werror` in `dev`. Defines `_DEFAULT_SOURCE` and `_POSIX_C_SOURCE=200809L`.
- Public names are prefixed `tix_` (types and functions) and `TIX_` (macros and enum
  values). File-local helpers are `static` and unprefixed.
- **Contracts in headers.** Every non-obvious public function documents REQUIRES / ENSURES
  / OWNS / BORROWS / RETURNS. Check preconditions with `TIX_REQUIRE` (always on, aborts with
  a report). This is for programmer errors only, never for bad input data.
- **Errors.** Use `tix_status` for conditions a caller can act on, plus a `tix_error *`
  out-parameter carrying a message. Untrusted or remote input never triggers an abort.
  It returns an error.
- **Memory.** `tix_x*` allocators abort on OOM. Ownership is explicit in the contract.
  Every `*_new`/`*_load` has a matching `*_free`. ASan and LSan must stay clean in tests.
- **Interfaces** are vtable structs embedded as the first member of the implementation,
  wrapped by `static inline` functions that check preconditions (loom pattern).
- **Determinism.** Anything that affects stored bytes or computed state (NDJSON key order,
  op ordering, cycle breaking, iteration over sets/maps) must be deterministic and
  independent of hash-table order, locale and time zone.

## Working with git.git (`libgit.a`)

- Pinned to **v2.47.3, the system git version**. Bump only together with the system git,
  and in its own commit.
- `cmake/GitLib.cmake` takes git's feature macros from its Makefile (`ALL_CFLAGS`). Never
  hand-copy or edit them. Struct layouts depend on them.
- In adapter sources: `#define USE_THE_REPOSITORY_VARIABLE`, then include
  `git-compat-util.h` **first**. `banned.h` forbids `strcpy`, `sprintf` and similar. Use
  `strbuf`/`xsnprintf`.
- A program linking libgit must provide `common_exit()` and initialise with
  `sanitize_stdfds(); initialize_repository(the_repository); setup_git_directory();
  git_config(git_default_config, NULL);` (see `tix_store_libgit_open`).
- After a child `git` has changed refs or objects, drop libgit's caches (the adapter's
  `forget_caches()`: release the main ref store, `reprepare_packed_git`). The files
  backend caches loose refs for iteration and would otherwise list stale values.
- Ref updates go through `ref_store_transaction_*` with an explicit old oid (CAS). A null
  old oid means "must not exist". Never update `refs/tix/*` without CAS.
- **Network is always a child `git` process** (`child_process` with `git_cmd = 1`), with
  argv only, never a shell, `--` before the remote, and only *configured* remote names
  (`tix_remote_name_valid`, then `remote_is_configured`), never URLs or paths. Capture
  stderr into `tix_error` with control characters replaced (servers write part of it).
- git's library calls `die()` freely, which exits the process. Keep such calls on paths
  where exiting is acceptable, and prefer the `*_gently` / error-returning variants where
  they exist.

## Data model invariants (don't break these)

- Op-packs are immutable. **Never rewrite history or force-push `refs/tix/*`.** A
  divergence is resolved by a merge commit.
- Entity id = root commit oid. Every `create` op carries a fresh 128-bit `nonce` (callers
  supply real randomness; `model` stays pure). Op id = `<commit oid>:<line>`. Total order =
  `(lamport, commit oid, line)` with one repo-wide Lamport clock, and a pack's Lamport value
  must exceed each parent's.
- The codec is canonical: decode re-encodes and rejects any byte difference. Changing an
  existing op's fields is a format change (new op name or header `tix` version).
- NDJSON: UTF-8, LF, final newline, header line first, keys in schema order.
- **Everything fetched from a remote is hostile until `model` has validated it**: size
  limits, strict schema, author rules, root = id in the ref name. A malformed pack rejects
  the whole incoming ref update and never crashes. Fetches land only in
  `refs/remotes/<remote>/tix/`; `refs/tix/` changes only by create, fast-forward or merge
  (core `tix_pull`). Local refs that don't validate are never pushed.
- Unknown op types are preserved and ignored, for forward compatibility.
- Bump `TIX_ENTITY_CODEC_VERSION` (model) whenever `tix_entity` or the replay rules change,
  so cached entities miss instead of serving stale state.
- WBS codes and summary rollups are derived, never stored.

## Testing

- `model`/`core`/`plan` tests run against `store_mem`. They are fast and deterministic, with
  no filesystem or git. `tix/sim.h` gives simulated clones (write, fetch, merge) for
  multi-clone scenarios. `tix/testkit.h` has `TK_CHECK`/`TK_OK`/`TK_STR_EQ` and a seeded PRNG.
- **Every index adapter must pass `tix_index_contract_run`** (`tix/index_contract.h`).
- `testkit`, `sim`, `store_contract` and `index_contract` are test-only modules. Only `TEST_DEPS` may name
  them, never `DEPS`.
- **Every store adapter must pass `tix_store_contract_run`** (`tix/store_contract.h`).
  New port behaviour gets a contract case first.
- store_libgit tests run one process per repository (libgit has global state). See
  `modules/store_libgit/tests/run-contract.sh`, which covers SHA-1/files and SHA-256/reftable.
- `plan` tests build entities directly (plain data) and assert golden dates. A change to
  scheduling semantics needs a golden case and the doc's §5 conventions updated. Keep
  `test_determinism` (shuffled input) passing.
- CLI behaviour is covered by `apps/cli/tests/cli.sh` (real repos, editor flow, clone
  round trip, a corrupt ref). Extend it with every new command or flag.
- **Convergence tests**: for any op set, every merge order and DAG shape must produce a
  byte-identical snapshot. New merge semantics need a convergence case.
- Sync is tested three ways: `core` tests on store_mem clones sharing a store_mem
  "server" (incl. hostile refs and a randomised convergence run), the store contract's
  network half (`tix_store_contract_run_remote`, against an empty bare peer), and
  `cli.sh` with real bare remotes.
- `store_libgit` tests use throwaway repos in `mktemp -d` with `GIT_CONFIG_GLOBAL=/dev/null`
  and `GIT_CONFIG_NOSYSTEM=1`. Never touch this repository's own refs from a test.
- Test names: `tests/<name>.c` in a module becomes CTest `<module>.<name>`.

## Commits

- Small, focused commits on `main`. Imperative subject ≤ 72 chars, a body that explains
  why, and milestone references where useful (`M1: …`).
- Update `docs/architecture.md`, this file and the README status table when a milestone
  or rule changes.
- Don't commit build output (`build/`), and don't modify `third_party/git` except to move
  the pin.
- Remote: `git@git.spacegoose.org:spacegoose/tix.git`. Never force-push `main`.

## Security

- Treat remote ticket data as untrusted input (it is parsed C on attacker-controlled bytes):
  bounds-check everything and fuzz parsers when they exist.
- Child processes: argv arrays only, no shell interpolation of user or remote strings.
- Never log secrets. Credential helpers and ssh keys belong to the child `git` process.
  tix never handles them.
