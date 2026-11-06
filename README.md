# tix

A distributed, git-native ticket tracker and project planner (in the spirit of git-bug).
Tickets, identities, resources and calendars live inside the git repository itself, in
`refs/tix/*`, and sync through any ordinary git remote (`tix sync`). Planning fields support a
work breakdown structure, Gantt scheduling and resource assignment.

Design: [`docs/architecture.md`](docs/architecture.md). Contributor/agent rules: [`AGENTS.md`](AGENTS.md).

## Status

- **M0 done**: links git.git's `libgit.a` (v2.47.3); in-process blob/tree/commit writes,
  compare-and-swap ref updates, SHA-256 + reftable repos, push/fetch round trip via `git`.
- **M1 done**: `base`, `store` port, `store_mem`, `model` (canonical NDJSON op-pack codec,
  DAG validation, Lamport ordering, LWW / observed-remove / comment merge rules for
  identities and tickets), with a randomised multi-clone convergence test.
- **M2 done**: `store_libgit` (real repos via libgit.a, passing the shared store contract),
  `core` (clock, identity, validated writes with CAS retry, queries) and the `tix` CLI.
- **M3 done**: `tix pull` / `push` / `sync`: fetch through a child `git`, validate every
  fetched entity, then create, fast-forward or merge; pushes are never forced. Verified
  against Forgejo with concurrent edits on two clones.
- Next: **M4**: planning (WBS, scheduling, resources, calendars, `plan`, `export`).

## Usage

```bash
tix id new                                # identity from git config user.name/email
tix new -t "Crash on save" -l bug         # or just `tix new` to use $EDITOR
tix ls                                    # active tickets (--all, --status, --label, --author)
tix show 7a3fe37
tix comment 7a3fe37 -m "looking into it"
tix edit 7a3fe37 --status in-progress +p1 -ui
tix sync                                  # pull + merge + push (origin, or git config tix.remote)
```

Also available as `git tix …`. A plain `git clone` doesn't copy tickets: run `tix pull`
once. Use `tix pull`/`push`/`sync` rather than raw `git push 'refs/tix/*'`: tix validates
what it fetches and merges diverged ticket histories, which plain git can't.

## Build

```bash
git clone --recurse-submodules git@git.spacegoose.org:spacegoose/tix.git
cd tix
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset release && cmake --build --preset release   # build/release/apps/cli/tix
```

Needs: CMake ≥ 3.28, Ninja, GCC/Clang (C17), GNU make, zlib headers.

## License

GPL-2.0-only (tix links git.git's `libgit.a`). See `LICENSE`.
