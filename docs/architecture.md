# tix — architecture

Status: **draft v0.1 (2026-11-06)**. Decisions marked ✅ are agreed; ❓ are open.

## 1. Goals

- ✅ Distributed, offline-first ticket tracker whose data lives **inside the git repo**, in
  custom refs (`refs/tix/*`), never in the working tree or on a branch (git-bug model).
- ✅ Concurrent offline edits to the same ticket merge **automatically and deterministically**:
  every clone that has the same set of commits computes the same ticket state.
- ✅ Written in **C (C17)**, linking **git's own library code** (`libgit.a` from git.git).
- ✅ Strict separation: a UI-agnostic core library; the CLI is just one thin frontend
  (TUI / web / bridges can come later without touching the core).
- Sync uses plain git remotes (Forgejo works unchanged: it stores and serves custom refs).

Non-goals for v1: bridges to Forgejo/GitHub issues, a web UI, attachments, permissions.

## 2. Layered structure

Ports-and-adapters ("hexagonal"). The domain is pure; everything that touches git, disk
or a terminal is an adapter behind a vtable interface.

```
            ┌──────────────────────────────────────────────┐
 frontends  │ apps/cli (tix)    apps/tui (later)  apps/web │  composition roots
            └───────────────────────┬──────────────────────┘
                                    │ only tix/core.h (+ adapter constructors in main)
            ┌───────────────────────▼──────────────────────┐
 service    │ core     use cases: create, edit, comment,   │
            │          show, list, pull/merge, push        │
            └───────┬──────────────────┬───────────────────┘
                    │                  │ interfaces (vtables)
            ┌───────▼───────┐  ┌───────▼────────┬────────────────┐
 domain     │ model         │  │ store (port)   │ index (port)   │
 (pure)     │ ops, replay,  │  │ objects, refs, │ query cache    │
            │ clocks, codec │  │ transport      │                │
            └───────┬───────┘  └───────┬────────┴───────┬────────┘
                    │                  │                │
            ┌───────▼──────────────────┴────────────────┴────────┐
            │ base   status, alloc, buffers, oid, contracts      │
            └────────────────────────────────────────────────────┘
 adapters   store_libgit (git.git libgit.a)   store_mem (tests)   index_sqlite
```

| Module | Depends on | Knows about git? | Contents |
|---|---|---|---|
| `base` | — | no | `tix_status`, `TIX_REQUIRE`, `tix_x*` allocators, `tix_buf`, `tix_oid` (opaque bytes + algo), string views |
| `model` | base | no | Operation types, op-pack codec (JSON), Lamport clocks, DAG linearisation, replay into `tix_ticket` state, per-field merge rules, validation |
| `store` | base | **interface only** | `tix_store` vtable: blob/tree/commit read+write, ref read/list-by-prefix/compare-and-swap update, merge-base, fetch/push |
| `index` | base, model | no | `tix_index` vtable: upsert ticket snapshot keyed by (id, head oid), query/filter |
| `core` | base, model, store, index | no | The public API every frontend uses (`tix_repo_open`, `tix_ticket_create`, `tix_ticket_apply`, `tix_list`, `tix_sync_pull`, …) |
| `store_libgit` | base, store, **libgit.a** | **yes — the only one** | Implements `tix_store` on git.git internals |
| `store_mem` | base, store | no | In-memory object/ref store for fast deterministic tests of model+core |
| `index_sqlite` | base, model, index, sqlite3 | no | `.git/tix/index.db`, disposable, rebuildable |
| `apps/cli` | core + adapter constructors | no | Arg parsing, formatting, `$EDITOR`, exit codes. **No business logic.** |

Rules (to be enforced at CMake configure time, as in `~/projects/loom`):

- A module may only `#include "tix/X.h"` for itself or declared `DEPS`.
- **No git.git header may appear outside `modules/store_libgit/src/`.** This quarantines
  git's global state, `die()`, its macro environment (`git-compat-util.h` must be the first
  include; `banned.h` poisons `strcpy`/`sprintf`/…) and its GPL internal API behind one seam.
- Library modules never print, never `exit()`, never read env/argv. They return `tix_status`
  plus a structured error; frontends decide how to present it.
- Core is synchronous and single-threaded in v1 (git.git's library code is not thread-safe).

## 3. On-disk data model

### 3.1 Refs

```
refs/tix/tickets/<ticket-id>          head commit of the ticket's operation DAG
refs/tix/identities/<identity-id>     ❓ v2: identity DAG (rename, keys) — v1 embeds name/email
refs/remotes/<remote>/tix/...         fetched copies, merged into refs/tix/* by `tix pull`
```

### 3.2 Ticket = DAG of commits, each carrying one op-pack

```
commit  (parents: previous op-pack commit(s); none for the root)
  author/committer: from git config (informational; authorship of record is in the ops)
  message: one-line human summary, e.g. "tix: set-status closed"
  tree
    ops               blob  canonical JSON op-pack (see 3.3)
    fmt-1             empty blob   format version marker
    lamport-<n>       empty blob   max Lamport time in this pack (readable without parsing)
```

- **Ticket id** = OID of the root commit (unique because it embeds author, time and the
  create op). Displayed as hex; any unique prefix ≥ 7 chars is accepted.
- Op-packs are immutable; edits are new ops. History is never rewritten or force-pushed.
- Merge commits (two parents) carry an empty op-pack; they only join histories.

### 3.3 Operations

Every op: `{"type", "lamport", "author": {"name","email"}, "time"(unix, informational), ...payload}`

| type | payload | merge rule |
|---|---|---|
| `create` | title, body, labels[] | exactly one, in the root pack |
| `set-title` | title | last-writer-wins (LWW) by (lamport, op-hash) |
| `set-status` | `open`/`closed` | LWW |
| `comment` | body → comment id = op hash | append-only |
| `edit-comment` | target comment id, body | LWW per target; author must match target author |
| `label` | add[], remove[] | observed-remove set (OR-set): a remove cancels only the adds it has seen |
| `assign` ❓ | add[], remove[] | OR-set |

**Ordering.** The repo keeps a Lamport clock in `.git/tix/clock`. Each new op gets
`max(seen)+1`; on fetch the clock is advanced to the max seen. Replay walks the DAG,
collects all ops, sorts by `(lamport, op-hash)` and folds them into state. Because the
ordering depends only on data in the commits, all clones converge.

**Validation (untrusted input).** Everything fetched from a remote is hostile until
validated: size limits on blobs/fields, strict schema, a `create` only at the root,
unknown op types preserved but ignored (forward compatibility), malformed packs reject the
whole ref update. ❓ Optional: require/verify signed commits (git's gpg/ssh signing).

### 3.4 Local, disposable state (`.git/tix/`)

- `clock` — Lamport clock.
- `index.db` — SQLite snapshot cache keyed by `(ticket id, head oid)`; rebuilt on mismatch.
  Makes `tix list` O(tickets) instead of O(all ops).

## 4. Sync

```
pull:  fetch  +refs/tix/*:refs/remotes/<remote>/tix/*
       for each ticket in remote namespace:
         local missing        → create local ref (after validation)
         remote ⊆ local       → nothing
         local ⊆ remote       → fast-forward (CAS ref update)
         diverged             → merge commit with both parents (empty op-pack)
push:  push refs/tix/*:refs/tix/*  (never forced); on rejection → pull, retry
```

Ref updates are compare-and-swap (`old oid → new oid`) so concurrent local tix processes
can't lose writes.

❓ Transport: in-process via git.git's transport code, or spawn `git fetch`/`git push`
from `store_libgit`. In-process is "pure", but the transport layer `die()`s freely and
pulls in credential helpers, ssh and remote-helper process management. Recommendation:
**object/ref access in-process; network via `git` subprocess in v1**, behind the same
`tix_store` interface so it can be swapped later.

## 5. Linking git.git (`libgit.a`)

git.git ships no supported library. Its Makefile builds `libgit.a` (plus `xdiff/lib.a`,
`reftable/libreftable.a`) for its own binaries, and recent git has an experimental
"libification" effort (`contrib/libgit-sys` / `libgit-rs`) that links against those archives.
Consequences we accept by choosing it:

| Concern | Impact | Mitigation |
|---|---|---|
| No stable API/ABI | Internal functions change between releases | Vendor git.git as a **git submodule pinned to a release tag**; build it from CMake (`ExternalProject`/custom target running git's Makefile); bump deliberately |
| Global state (`the_repository`) | One repo per process | Fine for the CLI; core API takes a `tix_repo*` anyway so a future backend can lift this |
| `die()` exits the process | Fatal for long-lived frontends (TUI/web) | v1: CLI only, acceptable. Adapter installs `set_die_routine`; ❓ later options: run store out-of-process, or contain via setjmp at the adapter boundary |
| Not thread-safe | Core is single-threaded | Documented in contracts |
| Macro environment / `banned.h` | Clashes with our headers | Headers quarantined to `store_libgit/src` (rule above) |
| **License: GPL-2.0-only** | tix as a combined work must be GPL-2.0-only | ✅/❓ confirm — choose GPL-2.0-only for the repo |

Benefit: exactly git's behaviour — config, reftable and files ref backends, SHA-1 and SHA-256
repos, packfiles, partial clone, signing — which libgit2 does not fully match.

First spike: build pinned `libgit.a`, link a 50-line program that opens the current repo,
writes a blob/tree/commit, and CAS-updates `refs/tix/test`. This proves the toolchain before
anything else is built.

## 6. Core API sketch (frontend-facing)

```c
typedef struct tix_repo tix_repo;            /* opaque: store + index + clock */

tix_status tix_repo_open(tix_repo **out, tix_store *store, tix_index *index, tix_error *err);
void       tix_repo_close(tix_repo *repo);

tix_status tix_ticket_create(tix_repo *, const tix_new_ticket *, tix_id *out, tix_error *);
tix_status tix_ticket_apply (tix_repo *, const tix_id *, const tix_op *ops, size_t n, tix_error *);
tix_status tix_ticket_load  (tix_repo *, const tix_id *, tix_ticket **out, tix_error *);
tix_status tix_ticket_resolve_prefix(tix_repo *, const char *prefix, tix_id *out, tix_error *);

tix_status tix_list(tix_repo *, const tix_query *, tix_ticket_iter **out, tix_error *);

tix_status tix_sync_pull(tix_repo *, const char *remote, tix_sync_report *out, tix_error *);
tix_status tix_sync_push(tix_repo *, const char *remote, tix_sync_report *out, tix_error *);
```

Frontends receive plain data (`tix_ticket` with title, status, labels, comments, timeline)
and never see oids of op-packs, refs or JSON.

## 7. CLI v1 (thin)

```
tix new [-t title] [-l label]...       (opens $EDITOR if no body)
tix ls [--status open|closed] [--label x] [--author y]
tix show <id>
tix comment <id> [-m msg]
tix edit <id> [--title t] [--status s] [+label] [-label]
tix pull [remote]   tix push [remote]   tix sync [remote]
```

Also installable as `git-tix` so `git tix …` works.

## 8. Build & quality

- C17, CMake + presets: `dev` (Debug, `-Werror`, ASan+UBSan), `release`.
- Module contracts documented in headers (REQUIRES / ENSURES / OWNS / BORROWS), as in loom.
- Tests: `model` and `core` tested against `store_mem` (fast, deterministic, including
  randomised convergence tests: apply the same ops in different DAG shapes → identical
  state); `store_libgit` tested against throwaway repos in a temp dir; CLI golden tests.
- JSON: ❓ cJSON (MIT, packaged) vs. a small hand-written canonical encoder in `model`.

## 9. Milestones

1. **M0 spike** — pinned git.git submodule, `libgit.a` linked, blob/commit/ref CAS demo.
2. **M1 domain** — `base`, `model`, `store` interface, `store_mem`; replay + merge rules with
   convergence tests.
3. **M2 core + CLI** — `core`, `store_libgit`, `tix new/ls/show/comment/edit`.
4. **M3 sync** — pull/merge/push against Forgejo.
5. **M4 index** — SQLite cache, query filters.
6. Later — identities DAG, signatures, TUI, Forgejo issue bridge.

## 10. Open questions

1. License: GPL-2.0-only (forced by linking libgit.a) — OK?
2. git.git pin: track the system's 2.47.x, or the latest release?
3. Transport: subprocess `git` for fetch/push in v1 (recommended) vs. in-process.
4. Identities: embedded name/email in v1, identity refs in v2 (recommended)?
5. JSON library: cJSON vs. hand-written canonical encoder.
