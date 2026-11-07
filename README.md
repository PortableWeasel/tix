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
- **M4 done**: project planning. WBS trees with fractional sibling ranks, FS/SS/FF/SF
  dependencies with lags, durations from effort and assignments, working-day calendars with
  holidays, resources with capacity, CPM scheduling with float and critical path, rollups,
  resource load, and `export` to Mermaid gantt, CSV and JSON.
- **M5 done**: a disposable SQLite cache of replayed entities (`$GIT_DIR/tix/index.db`),
  which makes listing and planning about 10× faster on warm runs (`tix index status|clear`).
- **M6 done**: `tix web`, a browser UI that needs no JavaScript: tickets, WBS, plan with an
  SVG Gantt, resource load, resources, calendars, sync, and forms for all of it. Loopback
  only, with a login URL printed at start; edits only write the fields you changed.
- **M7 done**: with JavaScript on, the web UI adds search as you type, ticket pickers,
  Gantt editing (drag to move, drag the end to resize, drag the handle to link, arrow
  keys), inline status changes, keyboard shortcuts (`?` lists them) and live refresh when
  the CLI or a sync changes something. Everything still works without it.
- In progress: plan baselines, MS Project XML export, commit signature enforcement, a
  Forgejo issue bridge, a TUI, and a hosted web mode behind SSO.

tix tracks its own work: `tix pull && tix wbs` in a clone of this repo.

## Usage

```bash
tix id new                                # identity from git config user.name/email
tix new -t "Crash on save" -l bug         # or just `tix new` to use $EDITOR
tix ls                                    # active tickets, in progress first (--all, --status, --label, --sort)
tix show 7a3fe37
tix comment 7a3fe37 -m "looking into it"
tix edit 7a3fe37 --status in-progress +p1 -ui
tix sync                                  # pull + merge + push (origin, or git config tix.remote)
tix web                                   # browser UI on 127.0.0.1:7480 (prints a login URL)

# planning
tix cal new "Site crew" --weekdays mon-sat --off 2026-12-24..2026-12-26=Holidays
tix res new Crew --kind role --capacity 300% --calendar <cal>
tix new -t House --calendar <cal> --deadline 2027-03-01
tix new -t "Dig foundations" --parent <house> --rank last --effort 48h
tix new -t "Pour slab" --parent <house> --rank last --duration 2d --dep +<dig>:FS+1d
tix assign <dig> <crew>
tix wbs <house>                           # tree with WBS codes and rollups
tix plan <house> --gantt --load           # ASCII Gantt and resource load (also --critical)
tix export <house> --format mermaid       # or csv, json
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
sudo cmake --install build/release --prefix /usr/local     # tix and git-tix
```

Needs: CMake ≥ 3.28, Ninja, GCC/Clang (C17), GNU make, zlib, cJSON and SQLite 3 headers.

## License

GPL-2.0-only (tix links git.git's `libgit.a`). See `LICENSE`.
