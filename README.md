# tix

A distributed, git-native ticket tracker and project planner (in the spirit of git-bug).
Tickets, identities, resources and calendars live inside the git repository itself, in
`refs/tix/*`, and sync with ordinary `git push`/`git fetch`. Planning fields support a
work breakdown structure, Gantt scheduling and resource assignment.

Design: [`docs/architecture.md`](docs/architecture.md). Contributor/agent rules: [`AGENTS.md`](AGENTS.md).

## Status

- **M0 done**: links git.git's `libgit.a` (v2.47.3); in-process blob/tree/commit writes,
  compare-and-swap ref updates, SHA-256 + reftable repos, push/fetch round trip via `git`.
- **M1 done**: `base`, `store` port, `store_mem`, `model` (canonical NDJSON op-pack codec,
  DAG validation, Lamport ordering, LWW / observed-remove / comment merge rules for
  identities and tickets), with a randomised multi-clone convergence test.
- Next: **M2**: `store_libgit`, `core`, CLI basics.

## Build

```bash
git clone --recurse-submodules git@git.spacegoose.org:spacegoose/tix.git
cd tix
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Needs: CMake ≥ 3.28, Ninja, GCC/Clang (C17), GNU make, zlib headers.

## License

GPL-2.0-only (tix links git.git's `libgit.a`). See `LICENSE`.
