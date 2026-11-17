# tix

A distributed, git-native ticket tracker and project planner. Tickets, identities, resources
and calendars live inside the git repository itself, in `refs/tix/*`, and sync through any
ordinary git remote (`tix sync`).

> **tix is based on [git-bug](https://github.com/git-bug/git-bug).** Most of its core design
> comes from git-bug, and the credit for those ideas belongs to the git-bug authors. tix is
> a separate reimplementation in C. It contains no git-bug code and is not affiliated with
> the git-bug project. If you want a mature, widely used git-native bug tracker, use git-bug.

Design: [`docs/architecture.md`](docs/architecture.md). Contributor/agent rules: [`AGENTS.md`](AGENTS.md).

## What tix takes from git-bug

These core ideas come from git-bug:

- **Data in custom refs, not in files.** Every entity is stored under its own ref
  (`refs/tix/tickets/<id>`, like git-bug's `refs/bugs/<id>`). Nothing goes in the working
  tree or on a branch, so tracking work never touches your code history.
- **Entities are DAGs of operations.** Each commit carries a pack of operations (create,
  set title, comment, label, …). An entity's state comes from replaying its operations, and
  its id is the hash of its root commit.
- **Lamport clocks and operation-based merging.** When two clones edit the same ticket,
  their histories are merged rather than overwritten. The operations are ordered by Lamport
  time and resolved by fixed rules, so every clone reaches the same result.
- **Identities are entities too.** Authors are versioned entities stored in the repo, kept
  separate from git's `user.name` and `user.email`.
- **Sync through plain git remotes.** No server is needed: push and pull the ticket refs to
  any git host.
- **The shape of the tool.** A `git <tool>` subcommand, a disposable local cache rebuilt
  from the refs, a web UI and a terminal UI, and bridges to hosted issue trackers.

tix differs in implementation: it is written in C17, links git.git's own `libgit.a` instead
of a Go git library, and encodes operation packs as canonical NDJSON (architecture §3).

## What tix adds

### Project planning

tix is a project planner as well as a ticket tracker. Tickets can carry planning fields:

- **Work breakdown structure:** parent/child trees with WBS codes, ordered siblings and
  rollups of effort, dates and progress (`tix wbs`).
- **Dependencies and scheduling:** finish-to-start, start-to-start, finish-to-finish and
  start-to-finish links with lags. Durations can be fixed or derived from effort and
  assignments. Critical-path scheduling computes float and the critical path (`tix plan
  --critical`).
- **Resources and calendars:** people or roles with a capacity, working-day calendars with
  holidays, assignments and resource load (`tix plan --load`). Resources and calendars are
  entities in their own right and sync like tickets.
- **Baselines:** freeze a plan (`tix baseline take`), then see what slipped (`tix plan
  --baseline B`).
- **Gantt charts and export:** ASCII Gantt in the terminal, an editable SVG Gantt and a
  dependency graph in the web UI, and export to Mermaid, CSV, JSON and MS Project XML.

### Workflows for AI agents

tix is built so that AI coding agents and people can share one backlog safely (architecture §15):

- **Phase/stage statuses:** a status is one of five fixed phases (`open`, `in-progress`,
  `blocked`, `done`, `cancelled`) plus an optional stage within it, such as `open/ready`.
  Each project defines its own stages (`tix stage`) and can mark stages that wait for a person.
- **Claims with leases:** an agent takes the next ready ticket with `tix claim --next`.
  When two agents race for the same ticket, the merge order decides who wins, and the loser
  backs off. The result is deterministic, even across clones that were offline.
- **Human-only stages:** each identity is marked as a human or an agent. With the autonomy
  guard on (`tix autonomy`), agent identities can't move tickets into stages marked for
  humans, such as review or approval.
- **Typed comments:** `tix note` adds comments that have a kind (such as `review`) and can
  carry structured JSON data, so tools can tell them apart from ordinary discussion.
- **Interfaces for agents and live use:** `--json` output, `tix log` and `tix watch`. In
  the web UI: an inbox, an activity feed, answer forms and live updates. A sample runner
  (`contrib/night.py`) drives agents through an unattended session.

### Other additions

- **Commit signatures:** sign ticket operations with your git ssh or gpg signing key and
  require valid signatures on fetch (`tix.sign`, `tix.verify`, `tix verify`).
- **Forgejo issue bridge:** `tix bridge sync --url https://host/owner/repo`.
- **A web UI that works without JavaScript**, with JavaScript as an enhancement, and a
  hosted mode for running it behind an SSO reverse proxy.

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
- The web UI's **Graph** page draws dependencies as a block diagram in sheets: one sheet
  per WBS level, group cards that open their own sheet, and connector tags for links that
  leave a sheet (architecture §13.10).
- After v1, done: MS Project XML export (`tix export --format mspdi`), plan baselines
  (`tix baseline take`, then `tix plan --baseline B` shows what slipped; also on the web),
  and commit signatures (`git config tix.sign true`, `tix id edit --add-signing-key`,
  `git config tix.verify require`, `tix verify`), a Forgejo issue bridge (`tix bridge sync
  --url https://host/owner/repo`), a terminal UI (`tix tui`), and a hosted web mode behind
  an SSO proxy (`tix web --trust-proxy X-authentik-username --public-url https://... --users
  FILE`; see docs/architecture.md §14.6).
- **Agent workflow (§15) done**: phase/stage statuses (`--status open/ready`, `tix stage`),
  claims with leases (`tix claim --next`), typed comments (`tix note`), an autonomy switch
  with a human-stage guard (`tix autonomy`), `--json`, `tix log` and `tix watch`, and in the
  web UI an inbox, an activity page, answer forms, live per-ticket events, timed pulls and a
  Forgejo push hook. The policy agents follow is docs/agent-workflow.md; contrib/night.py
  runs agents through an autonomous night.

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
tix warnings <ticket>                     # scheduling warnings about a ticket (or all)
tix export <house> --format mermaid       # or csv, json, mspdi (MS Project XML)
```

Also available as `git tix …`. A plain `git clone` doesn't copy tickets: run `tix pull`
once. Use `tix pull`/`push`/`sync` rather than raw `git push 'refs/tix/*'`: tix validates
what it fetches and merges diverged ticket histories, which plain git can't.

## Build

```bash
git clone --recurse-submodules https://github.com/PortableWeasel/tix.git
cd tix
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset release && cmake --build --preset release   # build/release/apps/cli/tix
sudo cmake --install build/release --prefix /usr/local     # tix and git-tix
```

Needs: CMake ≥ 3.28, Ninja, GCC/Clang (C17), GNU make, zlib, cJSON and SQLite 3 headers.

## License

GPL-2.0-only. See `LICENSE`.

tix statically links two GPL-2.0-only components, git.git's `libgit.a` and Cesanta
Mongoose, so it can't use git-bug's license (GPL-3.0): the two versions are incompatible.
tix shares git-bug's design but none of its code.
