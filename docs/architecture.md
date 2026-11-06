# tix — architecture

Status: **v1.0 (2026-11-06) — agreed baseline for implementation**. ✅ = agreed, ❓ = deferred beyond v1.

## 1. Goals

- ✅ Distributed, offline-first ticket tracker whose data lives **inside the git repo** in
  custom refs (`refs/tix/*`), never in the working tree or on a branch (git-bug model).
- ✅ Concurrent offline edits merge **automatically and deterministically**: every clone with
  the same commits computes the same state.
- ✅ C17, linking **git.git's own `libgit.a`**, pinned to the system git version (**v2.47.3**).
  ✅ License therefore **GPL-2.0-only**.
- ✅ Strict separation: UI-agnostic core libraries; the CLI is one thin frontend. TUI, web and
  bridges can come later without touching the core.
- ✅ Project-planning data good enough for **Gantt charts, a work breakdown structure (WBS)
  and resource definitions/assignments**.
- Sync uses plain git remotes. Forgejo needs no changes: it stores and serves custom refs.

Non-goals for v1: bridges to Forgejo/GitHub issues, a web UI, attachments, permissions,
automatic resource levelling, cost accounting beyond simple rate × effort.

## 2. Layered structure

Ports-and-adapters. Domain and planning logic are pure. Everything that touches git, disk,
processes or a terminal is an adapter behind a vtable interface.

```
            ┌──────────────────────────────────────────────────┐
 frontends  │ apps/cli (tix, git-tix)   apps/tui, apps/web later│  composition roots
            └────────────────────────┬─────────────────────────┘
                                     │ tix/core.h, tix/export.h (+ adapter ctors in main)
            ┌────────────────────────▼─────────────────────────┐
 service    │ core     use cases: entities CRUD, sync, plan    │
            └──┬──────────────┬──────────────┬──────────────┬──┘
               │              │              │              │
            ┌──▼─────┐  ┌─────▼─────┐  ┌─────▼─────┐  ┌─────▼──────┐
 domain     │ model  │◄─┤ plan      │  │ store     │  │ index      │  export ◄─ plan
 (pure)     │entities│  │ WBS, CPM, │  │ (port)    │  │ (port)     │  (Mermaid,
            │ops,CRDT│  │ load      │  │ objs/refs │  │ query cache│   CSV, JSON)
            └──┬─────┘  └─────┬─────┘  └─────┬─────┘  └─────┬──────┘
            ┌──▼──────────────▼──────────────▼──────────────▼──────┐
            │ base   status, alloc, buf, oid, date, ndjson, contracts│
            └──────────────────────────────────────────────────────┘
 adapters   store_libgit (libgit.a + `git` subprocess for network)
            store_mem (tests)   index_sqlite
```

| Module | Depends on | Contents |
|---|---|---|
| `base` | — | `tix_status`/`tix_error`, `TIX_REQUIRE`, `tix_x*` allocators, `TIX_VEC`, sorted string sets, `tix_buf`, `tix_oid` (opaque bytes + algo), canonical JSON writer, strict UTF-8 check. `tix_date` and durations arrive with M4 |
| `model` | base (+ cJSON, private) | Generic **entity framework** (DAG of op-packs, Lamport ordering, replay) plus the entity types: identity, ticket (M1), resource, calendar (M4). Op schemas, canonical codec, validation, merge rules. cJSON is used only to *parse*, inside `src/` |
| `plan` | base, model | Pure scheduling: WBS tree, rollups, working-day calendars, CPM (critical path method), critical path, resource load / over-allocation |
| `store` | base | `tix_store` vtable: op-pack commit write/read (the adapter owns the one-blob tree layout), ref read / list-by-prefix / compare-and-swap (CAS) update, per-clone local state, and (M3) `remote_fetch` / `remote_push` to a configured remote |
| `index` | base, model | `tix_index` vtable (M5 ✅): cache of replayed entities, keyed by (type, entity id); a lookup hits only when the stored head equals the ref's current head |
| `core` | base, model, plan, store, index | The public API frontends use |
| `input` | base, model, core | (M6 ✅) The text forms of field values shared by frontends: dates, `3d`/`16h`/`40%`, `+ID:FS+2d`, ranks, weekdays, ranges, rates. Field name + text → ops, with `<field>: …` messages |
| `export` | base, model, plan | Render a computed plan to Mermaid gantt, CSV, JSON. No I/O: writes into a `tix_buf` |
| `store_libgit` | base, store, **libgit.a** | The **only** module allowed git.git headers |
| `store_mem` | base, store | In-memory store for fast, deterministic model/core/plan tests; content-addressed with a non-cryptographic hash; simulated fetch |
| `index_mem`, `index_contract` | base, index | In-memory cache for core tests; the shared index contract (test-only) |
| `testkit`, `sim` | (tests only) | Check macros + PRNG; simulated clones (store_mem + clock + identity) that write, fetch and merge like core/sync will |
| `index_sqlite` | base, index, sqlite3 | `$GIT_DIR/tix/index.db`, disposable and rebuildable. The only module that includes `sqlite3.h` |
| `apps/cli` | core, input, export + adapter ctors | Arg parsing, formatting, ASCII Gantt/tree, `$EDITOR`, exit codes. **No business logic** |

Rules (enforced at CMake configure time, as in `~/projects/loom`):

- A module may only `#include "tix/X.h"` for itself or declared `DEPS`.
- **No git.git header outside `modules/store_libgit/src/`.** This keeps git's global state,
  `die()`, its macro environment (`git-compat-util.h` first, `banned.h`) and its unstable
  internal API behind one seam.
- Library modules never print, never `exit()`, never read env/argv/clock. "Now" and the local
  identity are passed in by the caller, so tests are deterministic.
- v1 is single-threaded (git.git's library code is not thread-safe).

## 3. Storage

### 3.1 Refs

```
refs/tix/identities/<id>      identity entity
refs/tix/tickets/<id>         ticket entity (issues, tasks, milestones, WBS nodes)
refs/tix/resources/<id>       resource entity (person, role, equipment)
refs/tix/calendars/<id>       working-time calendar entity
refs/remotes/<remote>/tix/…   fetched copies; merged into refs/tix/* by `tix pull`
```

### 3.2 Entity = DAG of commits, each carrying one op-pack

All four entity types share one mechanism (`model/entity`). Each type registers its op
schemas and a reducer.

```
commit   parents: previous op-pack commit(s); none for the root
         author/committer: from git config (informational only)
         message: one-line summary, e.g. "tix ticket: set-status done"
  tree
    ops.ndjson    blob — the op-pack
```

- **Entity id** = OID of the root commit. Shown as hex; any unique prefix of 7+ characters
  is accepted. The id is unique per type namespace. Every `create` op carries a 128-bit
  random **`nonce`**, so two otherwise identical creates (same content, author, second and
  Lamport value) still get distinct ids. The M1 convergence test caught exactly that collapse.
- Op-packs are immutable. History is never rewritten or force-pushed.
- Merge commits (two or more parents) carry a header-only pack (no ops). They just join
  histories. Their author is whoever merged, and the identity author rule (§4.1) doesn't
  apply to them.
- A pack's Lamport value must be **strictly greater than each parent's**. The total order
  below is then a linear extension of causality: a later edit always sorts after what it saw.

### 3.3 Op-pack format: NDJSON ✅

UTF-8, one JSON object per line, LF line endings, final newline, keys in a fixed order
(the encoder builds objects in schema order, so the output is byte-stable). Line 1 is the
header; every following line is one operation.

```
{"tix":1,"type":"ticket","author":"3f9c1e…","lamport":42,"time":1762430000}
{"op":"create","nonce":"9f86d081884c7d659a2feaa0c55ad015","title":"Pour foundations","body":"…","labels":["site"]}
{"op":"set-parent","parent":"a71d0b…"}
{"op":"dep-add","pred":"c02e9f…","kind":"FS","lag":2}
```

- `author` is an **identity id** ✅. `lamport` is per pack. `time` (unix) is informational and
  only used for actuals and display.
- **Op id** = `<commit oid>:<line number>`. It is content-addressed through the commit, so it
  is identical on every clone and needs no hashing in `model`. Comment ids are op ids.
- **Total order** for replay: `(lamport, commit oid, line)`. A single **repo-wide** Lamport
  clock (`.git/tix/clock`) is shared by all entity types. That makes cross-entity
  tie-breaks (WBS cycle breaking, §5.2) deterministic. Clock = max(seen) + 1 on write, and is
  advanced past everything seen on fetch.
- **Canonical form is enforced on read** ✅. Decoding re-encodes every known line and rejects
  any byte difference: whitespace, key order, extra or duplicate keys, `1.0`/`1e0`, `\u`
  escapes of printable characters, unsorted or duplicate set members. One logical pack has
  exactly one byte form, and no parser leniency can be exploited.
- Unknown op *names* are kept verbatim and ignored, for forward compatibility. Adding a
  field to an existing op is therefore a format change: it needs a new op name, or `tix: 2`
  in the header (readers return "unsupported" rather than guess). A malformed pack rejects
  the whole incoming ref update.
- Field rules: titles 1–256 bytes, single line. Bodies and comments ≤ 64 KiB, which may contain
  `\n`/`\t`. Labels are 1–64 bytes with no spaces or commas, at most 64 per op. Names ≤ 256.
  A pack is ≤ 1 MiB and ≤ 1000 ops. An entity is ≤ 20 000 commits. All text is strict UTF-8
  with no NUL and no other control characters.

- **Planning scalars (M4 ✅)** share one table-driven op shape, `{"op":"set-<field>","<key>":<value>}`
  (`set-start`, `set-duration`, `set-capacity`, …; `model/src/fields.c`). Each field has a
  value kind (date `"YYYY-MM-DD"`, integer, bool, enum string, id, string), a range, and
  whether `null` (unset) is allowed. Numbers are integers: durations and lags in working
  days, effort in hours, units and capacity in percent (100 = one full-time unit), rates in
  minor currency units per hour. Older readers see these as unknown ops and skip them.

### 3.4 Merge primitives (CRDT building blocks in `model`)

| Primitive | Semantics | Used for |
|---|---|---|
| LWW register | latest op in total order wins; may be set to `null` | scalars |
| OR-set | observed-remove: a remove cancels exactly the adds that are its causal ancestors (an ancestor commit, or an earlier line in the same commit). Concurrent add + remove → present | labels, keys, exceptions |
| OR-map | OR-set of keys, each value an LWW register | dependencies, assignments |
| append log | ordered by total order; `edit-comment` is LWW per comment and applies only when its author is the comment's author (otherwise ignored, identically on every clone) | comments |

## 4. Entities

### 4.1 Identity — `refs/tix/identities/<id>` ✅ in v1

| Field | Type | Merge |
|---|---|---|
| name, email | string | LWW |
| keys | `{kind: ssh\|gpg, fingerprint, pubkey}` | OR-set |
| (protected) | — | ops on an identity must be authored by that identity; `create` is implicitly self-authored |

- The local user is chosen with git config `tix.identity = <id>` (read through libgit's config
  API). `tix id new` creates one; `tix id use <id>` adopts an existing one on a new clone.
- ❓ Signature verification of op-pack commits against `keys` is planned but not enforced in v1.
  Until it is, the author-must-match rule is advisory: anyone with push access can forge it.

### 4.2 Ticket — `refs/tix/tickets/<id>` (planning fields: M4 ✅)

Tickets are both tracker items and plan nodes. A ticket with no parent is a **WBS root**
(a project).

**Tracking**

| Field | Type | Merge |
|---|---|---|
| title | string | LWW |
| body | markdown | LWW |
| status | `open` \| `in-progress` \| `blocked` \| `done` \| `cancelled` (✅ fixed set) | LWW |
| labels | strings | OR-set |
| comments | `{body}` + `edit-comment` | append log; edit = LWW per comment, author must match |

**Work breakdown structure**

| Field | Type | Merge | Notes |
|---|---|---|---|
| parent | ticket id \| null | LWW | builds the WBS tree |
| rank | fractional index string | LWW | sibling order; ties broken by id. Moves never rewrite siblings |
| kind | `task` \| `milestone` | LWW | *summary* is derived (has children), not stored |
| calendar | calendar id \| null | LWW | inherited down the WBS; default is built-in Mon–Fri 8h |

The WBS code (`1.3.2`) is **derived** from tree position and never stored, so concurrent moves
can't leave codes out of date.

**Scheduling (Gantt)**

| Field | Type | Merge | Notes |
|---|---|---|---|
| mode | `auto` \| `manual` | LWW | manual = use start/finish as drawn |
| start | date \| null | LWW | auto: "start no earlier than"; manual: fixed start |
| finish | date \| null | LWW | manual: fixed finish (ignored in auto) |
| duration | working days \| null | LWW | `0` for milestones |
| effort | hours \| null | LWW | used to derive duration when duration is unset (§5.3) |
| deadline | date \| null | LWW | never moves the task; violations are reported |
| progress | 0–100 | LWW | rolled up for summaries |
| actual_start / actual_finish | date \| null | LWW | if unset, derived from the first `in-progress` / last `done` op time |
| deps | key `pred id` → `{kind: FS\|SS\|FF\|SF, lag: ±N working days}` | OR-map | stored on the successor; ops `dep-add` / `dep-remove` |

**Resource assignments**

| Field | Type | Merge |
|---|---|---|
| assignments | key `resource id` → `{units: % of capacity, effort: hours\|null}` | OR-map (`assign` / `unassign`) |

`rank` is a fractional index over `[0-9a-z]` that never ends in `0`
(`tix_rank_between`), at most 64 characters. "Started" and "done" times are derived from
the first `set-status in-progress` and the `set-status done` that made the ticket done.
They are reported as actuals when `actual_start` / `actual_finish` are unset.

### 4.3 Resource — `refs/tix/resources/<id>` (M4 ✅)

| Field | Type | Merge | Notes |
|---|---|---|---|
| name | string | LWW | |
| kind | `person` \| `role` \| `equipment` | LWW | `role` = a generic pool ("2 × backend dev") |
| capacity | units (1.0 = one full-time unit) | LWW | a role can be 3.0 |
| calendar | calendar id \| null | LWW | own working time / leave |
| identity | identity id \| null | LWW | links a person resource to a user ("assigned to me") |
| rate, currency | minor units per hour \| null; ISO-style 3 letters \| null | LWW | optional cost estimate (two fields) |
| active | bool | LWW | archive without deleting |

✅ v1 has no edit control on resources or calendars (same as tickets): any author may edit.

### 4.4 Calendar — `refs/tix/calendars/<id>` (M4 ✅)

| Field | Type | Merge |
|---|---|---|
| name | string | LWW |
| weekdays | working-day mask (Mon–Sun) | LWW |
| hours_per_day | number | LWW |
| exceptions | `{from, to, working: bool, note}` (holidays, leave, extra days) | OR-map keyed by the `from..to` range; where ranges overlap, the later add wins |

A calendar with no working weekdays is unusable. `plan` falls back to the inherited calendar
and reports it.

### 4.5 Cross-entity references

References (parent, pred, resource, calendar, identity) are just ids. A reference to an
entity that hasn't been fetched yet is kept and shown as *unresolved*; it is never an error
at write or merge time. Integrity problems (cycles, dangling refs) are handled at **read time**
in `plan`, deterministically (§5.2).

## 5. Planning engine (`plan`, pure; M4 ✅)

Input: snapshots of tickets, resources and calendars for one WBS root, plus "today".
Output: `tix_schedule` (per-ticket computed dates, float, critical flag, WBS code, rollups,
per-resource daily load, diagnostics).

1. **WBS tree.** Build it from `parent`. A missing parent is treated as root level, with a
   diagnostic.
2. **Cycle breaking.** Concurrent `set-parent` ops on different clones can form cycles
   (A→B on one clone, B→A on another). Drop the cycle edge whose op is greatest in total
   order. Every clone drops the same edge, and a diagnostic is reported. Dependency cycles
   are broken the same way and also reported.
3. **Duration** ✅. Use `duration` if set. Otherwise `effort / Σ(assignment units × capacity ×
   calendar hours/day)`, rounded up to whole working days. Otherwise, if both ends are
   fixed (actual start and finish, or a manual task's start and finish), the working days
   between them. Otherwise the task is *unscheduled* (listed but not drawn). Milestones are 0.
4. **CPM forward/backward pass** on working-day calendars (✅ whole working days in v1; hours only for effort) (the task's inherited calendar)
   gives early/late start and finish, total float and the critical path. Inputs are
   FS/SS/FF/SF dependencies with lag, `start` (start-no-earlier-than) and the parent's start.
   Manual tasks are fixed points. Summary tasks span min(child start) to max(child finish).
   Their effort is summed and their progress is effort-weighted.
5. **Resource load.** Per resource per working day, Σ units assigned compared to capacity,
   giving an over-allocation report. No automatic levelling in v1.
6. **Variance.** Deadline misses, plus actual vs. planned dates.

Conventions as implemented (`tix/plan.h` has the full contract):

- A task of D ≥ 1 working days starting on working day S covers D working days of its
  calendar. `finish` is the last of them, inclusive. Milestones have start = finish: the
  finish day of what they wait for, or the next working day when nothing drives them.
- Lags are working days of the successor's calendar.
- Project start is the earliest `start` or `actual_start` in the plan, else "today", which
  the caller passes in.
- Dependencies on or of a summary apply to every leaf below it.
- Explicit actual dates fix a task. Derived actuals are only reported.
- Unscheduled tasks (no duration, no usable effort/assignments, no fixed start and finish)
  are listed but not drawn.
  Dependencies touching them are ignored, with a diagnostic.
- Resource load is in percent of capacity per day. More than 100 % on a working day, or
  any load on the resource's day off, is an over-allocation. The load view covers at
  most 3660 days.
- Summary progress is weighted by effort (or by duration × 8 h when effort is unknown).
- Results are independent of input order. That is tested by shuffling.

❓ Baselines (named snapshots of planned dates for planned-vs-actual Gantt bars) come later.
They can be stored as a `baseline` op-pack on the root ticket.

## 6. Sync ✅ via `git` subprocess (M3 ✅)

```
pull:  git fetch --prune -- <remote> +refs/tix/*:refs/remotes/<remote>/tix/*
       for each fetched ref of a known type (identities first, then tickets):
         replay the remote history: schema, sizes, author rules, root == id in name
           invalid           → rejected: reported, never adopted
         local missing       → create local ref (CAS, old = none)
         remote ⊆ local      → nothing (we are ahead)
         local ⊆ remote      → fast-forward (CAS)
         diverged            → merge commit, parents (local, remote), header-only pack,
                               authored by the identity (or null); the union is replayed
                               again before the CAS (limits apply to the merged entity)
       advance and save the Lamport clock
push:  every valid local entity ref that differs from refs/remotes/<remote>/tix/…
       git push --porcelain -- <remote> <ref>:<ref>…     (never '+', never forced)
       then record the pushed values in refs/remotes/<remote>/tix/…
       "[rejected]" → CONFLICT (pull, retry); "[remote rejected]" (hook/policy) → error
sync:  pull, push; on CONFLICT pull + push again (3 rounds at most)
```

- Object and ref access run in-process through libgit.a. Network goes through a child `git`
  process (the same binary version as the pinned library). Credential helpers, ssh config and
  remote helpers then behave exactly as the user expects, and a transport `die()` can't take
  down tix.
- The child is spawned with argv directly, never through a shell, with `--` before the
  remote. Only **configured remote names** are accepted (`[A-Za-z0-9._-]`, not starting
  with `-` or `.`); URLs and paths are refused. Its stderr is captured into `tix_error`,
  with control characters replaced because the server writes part of it.
- Fetch also passes `--no-tags --no-write-fetch-head --no-recurse-submodules`: it touches
  nothing but the tix mirror namespace. Push passes `--no-verify`.
- After the child returns, store_libgit drops libgit's ref store and re-scans packs, since
  the files backend caches loose refs for iteration.
- Pushes are **not atomic**: refs that fast-forward land even when others are rejected.
  That is safe (each ref is independent) and means progress under contention.
- A local ref whose last-known remote value isn't in its history is not pushed. Right after a
  pull that only happens when the remote's history was rejected, so `sync` reports it
  instead of looping. Local refs that don't validate are never pushed either.
- A remote ref of an unknown type (`refs/tix/<newer kind>/…`) is counted and ignored. That's
  forward compatibility, the same as unknown ops.
- Merge commits differ per clone (time, author, Lamport value), so two clones that merge the
  same pair produce two merges that a later sync joins with a third. That is harmless: merge
  packs carry no ops, and snapshots are identical.
- Known limit: a remote branch literally named `tix` would make git's own default refspec
  map it to `refs/remotes/<remote>/tix`, which collides with the mirror namespace (a
  directory/file conflict). git then refuses one of the two fetches; tix reports git's error.
- Verified on 2026-11-06 against Forgejo (a private throwaway repo over ssh): two clones,
  concurrent edits, push refused, `sync` merged, both clones show identical tickets, and
  `fsck --strict` is clean. Forgejo stores and serves `refs/tix/*` unmodified.

## 7. Linking git.git (`libgit.a`, v2.47.3)

git.git ships no supported library. Its Makefile builds `libgit.a`, `xdiff/lib.a` and
`reftable/libreftable.a` for its own binaries.

| Concern | Mitigation |
|---|---|
| No stable API | git.git as a **submodule pinned at `v2.47.3`**, built by CMake via git's Makefile; bumped deliberately, together with the system git |
| Global state (`the_repository`) | One repo per process; the core API still takes a `tix_repo*` |
| `die()` exits | Network is out of process (§6). Adapter sets `set_die_routine` to a reporter. Long-lived frontends (TUI/web) ❓ may need the store out of process later |
| Not thread-safe | v1 single-threaded |
| Macros / `banned.h` | Headers quarantined in `store_libgit/src` |
| GPL-2.0-only | ✅ accepted; `LICENSE` = GPL-2.0-only |

### 7.1 M0 findings (2026-11-06) ✅

- `make libgit.a xdiff/lib.a reftable/libreftable.a` (≈35 s, 4 cores) links into a standalone
  program with **only one missing symbol, `common_exit()`**, normally from `common-main.c`.
  The adapter defines it. The archives reference each other and are linked as a rescan group.
- Headers need git's feature macros (`SHA1_DC`, `SHA256_BLK`, `HAVE_*`, `NO_*`). CMake reads
  them from git's own `ALL_CFLAGS` at configure time, and the headers are included `-isystem`.
- Init sequence: `sanitize_stdfds()`, `initialize_repository(the_repository)`,
  `setup_git_directory()`, `git_config(git_default_config, NULL)`.
- APIs used: `write_object_file`, `commit_tree` (author/committer default from config),
  `ref_store_transaction_begin` / `ref_transaction_update(old_oid)` / `_commit` for CAS
  (null old oid = must not exist), `get_tree_entry`, `repo_read_object_file`,
  `run_command` with `git_cmd = 1` for fetch/push.
- Verified by the M0 spike (removed in M3, once `store_libgit` and `tix sync` covered the
  same ground): CAS refusal, `fsck --strict` clean, two-clone round trip through a bare
  remote, and a **SHA-256 + reftable** repo.

## 8. Core API (`tix/core.h`, M2 ✅)

The full contract lives in the header. In outline:

```c
typedef struct tix_env { int64_t (*now)(void *); void (*random)(void *, uint8_t *, size_t); void *ctx; } tix_env;
typedef struct tix_repo_config { tix_store *store; tix_env env; tix_oid identity; } tix_repo_config;

tix_status tix_repo_open(tix_repo **out, const tix_repo_config *cfg, tix_error *err);
tix_status tix_repo_set_identity(tix_repo *, const tix_oid *id, tix_error *);   /* in memory */

tix_status tix_entity_resolve(tix_repo *, tix_entity_type, const char *prefix, tix_oid *out, tix_error *);
tix_status tix_entity_load   (tix_repo *, tix_entity_type, const tix_oid *id, tix_entity **out, tix_error *);
tix_status tix_entity_create (tix_repo *, tix_entity_type, tix_op *ops, size_t n, tix_oid *out, tix_error *);
tix_status tix_entity_apply  (tix_repo *, tix_entity_type, const tix_oid *id, const tix_op *ops, size_t n, tix_error *);

tix_status tix_ticket_list  (tix_repo *, const tix_ticket_query *q, tix_entity_list *out, tix_error *);
tix_status tix_identity_list(tix_repo *, tix_entity_list *out, tix_error *);

/* M3 */
tix_status tix_pull(tix_repo *, const char *remote, tix_sync_report *rep, tix_error *);
tix_status tix_push(tix_repo *, const char *remote, tix_sync_report *rep, tix_error *);
tix_status tix_sync(tix_repo *, const char *remote, tix_sync_report *rep, tix_error *);
```

`tix_sync_report` counts created / forwarded / merged / rejected / skipped / pushed /
unpushable refs and carries one problem line per rejected or unpushable ref. A rejected
ref is not an error status: the rest of the sync still happens.

M4 ✅ adds `tix_resource_list`, `tix_calendar_list`, `tix_resource_create`,
`tix_calendar_create` and `tix_ticket_rank` (first / last / before / after, never
rewriting siblings), plus `tix_plan(r, root, today, &result)`. The ticket query gains
`assignee` and `under`. Up-front checks on local edits: references must resolve to an
entity of the right type, and parent or dependency cycles are refused. Removing an absent
dependency, assignment or exception is NOT_FOUND. Remote data is never rejected for
these: concurrent edits can still form cycles, which `plan` breaks deterministically.

- **Inputs from outside**: the clock (`now`), randomness (create-op nonces) and the chosen
  identity all come in through `tix_repo_config`. Core never reads env, time or config itself.
- **Local state** goes through the store port (`local_get`/`local_set`). The Lamport clock is
  `$GIT_DIR/tix/clock` in store_libgit, written under git's lockfile.
- **Long-running frontends** (M6 ✅) call `tix_repo_refresh` before each unit of work. It runs
  the store's `refresh` (store_libgit: drop the ref store, re-prepare packs; a store contract
  case, plus a test where a real `git update-ref`/`pack-refs` moves refs under an open store)
  and re-reads the Lamport clock, which never moves backwards.
- **Identity choice** is git config `tix.identity`. The frontend reads and writes it through
  `tix_store_libgit_config_get/_set` and hands it to core.
- **Writes** validate the current history first (never build on invalid data), write one pack
  with lamport = clock + 1, and CAS the ref. A lost CAS is retried, up to 5 attempts.
- **Up-front rule checks** for things model would silently ignore: editing another
  identity, or editing a comment that isn't yours or doesn't exist.
- **Listing** loads every entity (O(all commits) until the M5 index). Refs that fail
  validation are skipped and counted (`nbroken`), never fatal. A ref whose history has a
  different root than its name is invalid.
- **Read models** are model's `tix_entity` (plain data). `TIX_ID_MIN_PREFIX` = 7.

### 8.1 Index cache (M5 ✅)

- **What is cached**: whole replayed entities, as `tix_entity_encode` bytes (lossless JSON,
  `model/src/codec.c`), under (type, id) together with the head they were replayed from.
  `tix_entity_load` reads the ref, and on a hit with the same head decodes instead of
  replaying. Any problem counts as a miss: an entry that doesn't decode, a different codec
  version, or an entry describing another entity. The entity is then replayed and the entry
  rewritten. Only entities that validated are ever stored.
- **What is not**: sync (`tix_pull` replays every fetched history) and writes (`apply`
  replays the current head before building on it) always validate in full. The cache only
  speeds up reads.
- `TIX_ENTITY_CODEC_VERSION` must be bumped whenever `tix_entity` or replay semantics
  change. Stale entries then miss instead of returning old results.
- **SQLite adapter**: one `WITHOUT ROWID` table, `PRAGMA user_version` as the schema
  version, WAL with `synchronous=OFF` (it is a cache). A file that isn't a usable tix cache
  is deleted and recreated. A symlink or non-file in its place is refused, and tix then runs
  without a cache. Busy timeout 2 s, so concurrent tix processes don't fail.
- **Queries** still filter decoded entities in core. With the decode cost this low, SQL
  queries weren't needed for the target size (thousands of tickets). An `entities`
  side table with indexed columns can come later without changing the port much.
- Measured (release build, 500 tickets × 5 commits, loose objects): `tix ls --all` 300 ms
  without the cache, 340 ms cold (filling it), 25 ms warm. After `git gc`: 50 ms → 21 ms.
- CLI: `tix index status|clear`. `TIX_NO_INDEX=1` disables the cache.

## 9. CLI v1

M2 ✅ implements `id`, `new`, `ls`, `show`, `comment` (incl. `--edit COMMENT-ID`) and `edit`.
M3 ✅ adds `pull`, `push` and `sync` (default remote: git config
`tix.remote`, else `origin`; any rejected or unpushable ref is a warning and exit 1).
M4 ✅ adds the planning options below, `assign`/`unassign`, `res`, `cal`, `wbs`, `plan`
and `export`. Dates accept `YYYY-MM-DD`, `today` and `tomorrow`; `none` clears a value.
`$TIX_TODAY` (or `--today`) fixes "today" for scripts. Plan diagnostics go to stderr as
`tix: note: …`. `tix plan --gantt` draws one column per day (per week beyond 120 days),
and `--load` draws per-resource daily load. `$EDITOR` resolution: `TIX_EDITOR`, then
`core.editor`, then `VISUAL`, then `EDITOR`, then `vi`. Lines starting with `#` are dropped. Exit codes: 0 ok, 1 error, 2 usage.

```
tix id    new | ls | show <id> | use <id> | edit <id> [--name] [--email] [+key/-key]
tix new   [-t title] [-l label]... [--parent P] [--kind milestone]
tix ls    [--status s] [--label l] [--assignee r] [--under P]
tix show  <id>
tix comment <id> [-m msg]
tix edit  <id> [--title] [--status] [+label|-label] [--parent P] [--rank first|last|before:X|after:X]
               [--kind task|milestone] [--calendar C] [--mode auto|manual]
               [--start D] [--finish D] [--duration 3d] [--effort 16h] [--deadline D]
               [--progress 40] [--actual-start D] [--actual-finish D] [--dep +P:FS+2d | -P]
tix assign <id> <resource>[:50%] [--effort 8h]     tix unassign <id> <resource>
tix res   new NAME | ls | show | edit   (--kind, --capacity 300%, --calendar, --identity ID|me,
                                         --rate "95.50 EUR", --name, --active yes|no)
tix cal   new NAME | ls | show | edit   (--weekdays mon-fri, --hours 8, --off D[..D][=NOTE],
                                         --on D[..D], --clear D[..D], --name)
tix wbs   [root]                       tree with WBS codes and rollups
tix plan  [root] [--gantt] [--critical] [--load]   ASCII Gantt / resource load
tix export [root] --format mermaid|csv|json
tix pull | push | sync [remote]
```

Also installed as `git-tix`, so `git tix …` works.

## 10. Build & quality

- C17, CMake + presets: `dev` (Debug, `-Werror`, ASan+UBSan), `release`.
- Contracts documented in headers (REQUIRES / ENSURES / OWNS / BORROWS), as in loom.
- **Convergence tests** (model): generate random op sequences and DAG shapes across simulated
  clones on `store_mem`; every merge order must give byte-identical snapshots.
  M1: 150 seeds × 3 clones × 60 random steps (≈ 8.4k ops, ≈ 2k merge commits, every fifth seed
  SHA-256), plus replay of each DAG in shuffled input order. Mutation check: making the
  tie-break depend on input order fails the test (33 divergences).
- **Plan tests**: CPM golden cases (all four dependency types, lag, calendars and exceptions,
  manual tasks, cycles); load tests for over-allocation.
- `store_libgit` is tested against throwaway repos, including a two-clone pull/push round trip
  through a bare repo. CLI golden tests.

## 11. Milestones

1. ✅ **M0 spike** (done 2026-11-06): git.git submodule at v2.47.3, `libgit.a` linked; write blob/tree/commit,
   CAS `refs/tix/test`, `git fetch/push` subprocess round trip.
2. ✅ **M1 domain** (done 2026-11-06): `base`, `model` entity framework + identity/ticket (tracking fields),
   `store_mem`, convergence tests.
3. ✅ **M2 core + CLI** (done 2026-11-06): `store_libgit`, `core`, `tix id/new/ls/show/comment/edit`.
4. ✅ **M3 sync** (done 2026-11-06): pull/merge/push against Forgejo; M0 spike removed.
5. ✅ **M4 planning** (done 2026-11-06): WBS/scheduling/assignment fields, resource and calendar
   entities, `plan`, `tix wbs/plan/assign/res/cal`, `export`.
6. ✅ **M5 index** (done 2026-11-06): SQLite cache of replayed entities, `tix index`.
7. ✅ **M6 web UI, no JavaScript** (done 2026-11-06, §13): `input` module, store refresh, Mongoose
   adapter, pages, forms, SVG Gantt, `tix web`.
8. **M7 web UI enhancements** (§13.4): search as you type, Gantt editing, live refresh.
9. Later: signature enforcement, baselines, TUI, Forgejo issue bridge, MS Project XML export,
   hosted web mode (§13.8).

## 12. Resolved decisions (2026-11-06)

| # | Question | Decision |
|---|---|---|
| 1 | Hosting / storage | Custom refs + op-pack DAG (git-bug model) |
| 2 | Language / git | C17, git.git `libgit.a` pinned to system git v2.47.3 |
| 3 | License | GPL-2.0-only |
| 4 | Network transport | `git` subprocess |
| 5 | Identities | Identity refs in v1 |
| 6 | Op-pack encoding | NDJSON |
| 7 | Status set | Fixed five values |
| 8 | Effort → duration | Simple formula (§5.3) |
| 9 | Time granularity | Whole working days; hours for effort only |
| 10 | Edit control on resources/calendars | None in v1 |
| 11 | Web UI server library | Cesanta Mongoose 7.23, vendored, single-threaded loop (§13.1) |
| 12 | Web UI deployment | Local per repo first; hosted behind SSO later (§13.8) |
| 13 | Web UI first milestone | Read + edit without JavaScript (M6); JS enhancements next (M7) |

## 13. Web UI (M6–M7, design 2026-11-06)

A spartan, server-rendered web UI: `tix web` serves the repository it runs in. **Every
function works without JavaScript.** Plain HTML forms, links and a server-drawn SVG Gantt
cover reading and editing. A small script, when it runs, makes the same things quicker
(search as you type, dragging Gantt bars) through the same endpoints. Local first. A hosted
mode is kept possible but not built (§13.8).

### 13.1 Library: Cesanta Mongoose 7.23

| Considered | Outcome |
|---|---|
| **Mongoose 7.23** (2026-08-12) | ✅ `GPL-2.0-only or commercial`, the same as tix. One `mg_mgr_poll` event loop, so every handler runs on our thread, which `libgit.a` needs. Two files. Has form/multipart/cookie helpers and chunked responses (SSE) |
| libmicrohttpd 1.0.10 | Fallback: an external-select mode that is also single-threaded, LGPL. Debian 13's 1.0.1 has unpatched CVEs, so it would be vendored too |
| facil.io 0.7 / 0.8 (cstl) | 0.7 is stale with an unpatched JSON CVE. 0.8 is very new and churning (5 releases in 3 weeks, High advisories 2026-11-03), and handlers may run on several threads |
| libevent evhttp, CivetWeb, h2o, Kore, lwan, libsoup, onion, own server on llhttp | No multipart/cookies (evhttp); a thread pool and no release since 2023 (CivetWeb); too large, a framework, Linux-only, GLib, unmaintained; or 1.5–3k lines of our own security-sensitive parsing |

- ✅ (M6) **Vendored** as the submodule `third_party/mongoose` at tag `7.23`
  (`02bdbb9ed6f0a8f7c42228c6e7cb35d748b60551`), built by CMake as a plain static library.
  It is checked to compile warning-free under C17 with tix's full warning set and
  ASan/UBSan. Bumped deliberately, after reading the advisories
  (github.com/cesanta/mongoose/security).
- **Configuration** (compile definitions, no `mongoose_config.h`): `MG_ARCH=MG_ARCH_UNIX`,
  `MG_TLS=MG_TLS_NONE`, `MG_ENABLE_TCPIP=0` (OS sockets, not Mongoose's own TCP/IP stack),
  `MG_ENABLE_POLL=1`, `MG_ENABLE_IPV6=1`, `MG_ENABLE_LOG=0`, `MG_ENABLE_DIRLIST=0`,
  `MG_ENABLE_SSI=0`. Most of Mongoose's 2025–26 CVEs are in its built-in TLS, TCP/IP,
  DNS and mDNS code. None of that is compiled in or called. We never call
  `mg_http_serve_dir` (no file serving), WebSockets, MQTT or the client APIs.
- **Quarantine**, like git.git: `mongoose.h` may only be included by
  `apps/web/src/http_mongoose.c`. Everything else sees `web_http.h`: a parsed request
  (method, path, decoded query and form fields, cookies, selected headers, peer address)
  and a response builder. Handlers are therefore plain functions from request to
  response, testable without sockets, and the library can be swapped behind one file.
  Mongoose only splits a request into method, path, query, headers and body. Decoding
  forms and cookies (with the limits of §13.6) is our own `request.c`, fuzzed in its tests.
  Bodies that are too large (413), chunked (501) or not forms (415) are refused from the
  headers alone, before the body is read.

### 13.2 Process and layering

```
tix web [--listen 127.0.0.1:7480] [--read-only] [--no-open]
  └─ execs tix-web (apps/web, same install dir, argv only)
       http_mongoose.c ─► router ─► handlers (pages, forms) ─► core, plan, export
                                         └─► html.c (escaping writer), svg_gantt.c, assets (embedded)
```

- `apps/web` builds a separate binary, `tix-web`, so the CLI never links HTTP code.
  `tix web` finds it next to its own executable and `execv`s it.
- **New module `input`** (base, model, core): the text grammar for planning values is
  moved out of `apps/cli/src/planopts.c` (dates incl. `today`/`tomorrow`, `3d`, `16h`,
  `40%`, `+ID:FS+2d`, ranks, `none`) and shared by CLI and web. It turns field strings
  into `tix_op` lists, so both frontends accept and reject exactly the same input. There
  is no business logic in either app.
- **One thread.** All core calls happen inside `mg_mgr_poll` on the main thread.
  `tix sync` from the browser blocks the loop while the child `git` runs. That is
  acceptable locally, and the page says so.
- **Staying fresh in a long-running process.** Other processes (the CLI, `git gc`)
  change refs and packs behind libgit's back. A new store operation `refresh` drops
  libgit's ref store and re-prepares packs (the existing `forget_caches`). The web app
  calls it at the start of every request (`tix_repo_refresh`, M6 ✅). The index cache stays
  correct because hits require the current head.
- **Long life.** libgit leaks are bounded per request and checked by an ASan soak test
  (thousands of requests). Any libgit path that can `die()` would kill the server, just
  as it ends a CLI command today. Such paths are bugs to be fixed in `store_libgit`.

### 13.3 Pages and endpoints (M6, no JavaScript; read pages ✅)

| Method + path | Purpose |
|---|---|
| `GET /` | Redirect to `/tickets` |
| `GET /tickets?q&status&label&assignee&under` | List with a filter form. `q` matches an id prefix or a title substring |
| `GET /t/ID` | Ticket: fields, planning, dependencies, assignments, comments. Forms: comment, status |
| `GET /new`, `POST /new` | Create (title, body, labels, all planning fields) |
| `GET /t/ID/edit`, `POST /t/ID` | Edit; only the fields that changed are written (§13.5) |
| `POST /t/ID/comment`, `POST /t/ID/comment/CID` | Add or edit a comment |
| `POST /t/ID/dep`, `POST /t/ID/assign` | Add or remove a dependency or an assignment |
| `GET /wbs?root` | WBS tree with codes, dates and rollups |
| `GET /plan?root&critical&today` | Plan table plus an inline SVG Gantt (bars link to tickets, critical path marked, dependency arrows) and diagnostics |
| `GET /load?root` | Resource load table, over-allocation highlighted |
| `GET /res`, `/res/ID`, `/cal`, `/cal/ID` (+ `new`/`edit` and `POST`) | Resources and calendars |
| `GET /export?root&format=mermaid\|csv\|json` | Download (`Content-Disposition: attachment`) |
| `GET /sync`, `POST /sync` | Pull + push with the report shown |
| `GET /id` | Identities; which one this server writes as |
| `GET /assets/<hash>.css`, `.js` | Embedded in the binary, immutable cache |

- **Post/Redirect/Get**: a successful POST redirects (303) to the entity. A rejected one
  re-renders the form with the user's values and the error messages (422). Errors come
  from `input` and core, with the same wording as the CLI.
- **Ticket search** (`q`) is core's `tix_ticket_query.text`: an id prefix or a title substring,
  ASCII case-insensitive. The default list shows active tickets; any filter replaces that.
- **Ticket pages** run the whole plan (dependencies reach outside any subtree) to show the
  computed dates, float, subtasks, successors and the inherited calendar.
- **Gantt SVG**: one column per day (24 px up to 45 days, 14 px up to 120), else per week.
  Classes only, no `style` attributes (the CSP forbids them); arrows for all four
  dependency kinds; bars are links with a `<title>` tooltip.
- **HTML**: semantic, with tables, `<label>` and native date inputs. It is usable in
  w3m/lynx (checked in tests with `w3m -dump` where available). One small CSS file and
  system fonts. Short ids are shown, with full ids in `title` and the URL.

### 13.4 JavaScript enhancements (M7)

One hand-written `app.js` (no framework, no build step, no third-party code) that each
page loads with `defer`:

- **Search as you type**: `GET /api/tickets?q=` returns JSON (id, title, status) for
  the id/title box and for ticket pickers (parent, dependency). Without JS these are
  the normal search form and a text field taking an id prefix.
- **Gantt editing**: drag a bar to move it (sets `start`; manual tasks also `finish`),
  drag its end to resize (`duration`), drag between bars to add an FS dependency. It
  submits the same form fields to `POST /t/ID` with `fetch` and re-renders the SVG from
  the response. Arrow keys work on a focused bar.
- **Inline status change, keyboard shortcuts**, and **live refresh** through SSE
  (`/events`, a ref-change poll on the server once a second) when the CLI or a sync
  changes something.
- Progressive enhancement only: every request JS makes is one a form can also make.

### 13.5 Concurrent edits from the browser

Edit forms carry the original value of every field (hidden `o.<field>`). On POST, a field
is written only when its submitted value differs from its original value. A stale tab
therefore never overwrites a field someone else changed meanwhile. If the entity's head
moved since the form was rendered (hidden `head`), and a field the user changed was
also changed by someone else, the form is re-shown with both values instead of
applying. Otherwise ops merge as usual (op-based CRDT).

### 13.6 Security

The server is on a multi-user machine, and its pages show data fetched from remotes.
Loopback alone doesn't keep other local users or web pages out.

- **Binding**: `127.0.0.1` (or `[::1]`) only in M6. `--listen` refuses other addresses
  until the hosted mode exists. The default port is 7480. If it is taken, the next free
  one is used and printed.
- **Session token**: on start, 32 random bytes from the core RNG. tix prints a URL
  `http://127.0.0.1:7480/login?t=…` and runs `xdg-open` on it if a desktop session exists (never with `--no-open`). From another
  machine, use `ssh -L 7480:127.0.0.1:7480 host`; the `Host` header then still matches.
  - `GET /login?t=…` checks the token, sets the cookie `tix_session` (HttpOnly;
    SameSite=Strict; Path=/) and answers 303 to `/`, so the token leaves the address bar
    at once. This works without JS. A URL fragment wouldn't, because only a script can
    read it.
  - The server never logs URLs, and `Referrer-Policy: same-origin` keeps the token out
    of requests to other sites. (`no-referrer` is not an option: browsers then send
    `Origin: null` on form posts, which the CSRF check must refuse. Found with a real
    browser, M6.) It stays only in the user's own browser history. A `/login`
    form for pasting the token covers a browser on another machine.
  - Every other request without a valid cookie gets 401, with the login form and a note
    that `tix web` printed the URL.
  - Tokens, session ids and form tokens are compared in constant time. The login token
    lives only in memory and in the printed URL: there is no token file to protect, and
    a restart prints a new one (M6 ✅).
  - Sessions live in memory: at most 16, the least recently used is dropped. Each has its
    own form token. Every page carries a log-out form.
- **CSRF**: every POST needs the session's form token in a field `csrf`, plus an `Origin`
  (or, without one, a `Referer`) matching the server exactly. SameSite=Strict is the third
  layer. Failures are 403 with a "reload and try again" page.
- **DNS rebinding**: the `Host` header must be exactly the listening `host:port`
  (`127.0.0.1`, `[::1]` or `localhost`). Anything else gets 421.
- **Response headers on every response**:
  - `Content-Security-Policy: default-src 'none'; style-src 'self'; script-src 'self';
    img-src 'self'; connect-src 'self'; form-action 'self'; frame-ancestors 'none';
    base-uri 'none'`
  - `X-Content-Type-Options: nosniff`, `Referrer-Policy: same-origin`,
    `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Resource-Policy: same-origin`
  - `Cache-Control: no-store` on pages
- **Output escaping by construction**: `html.c` has `h_text`, `h_attr` and `h_url`
  writers. Handlers have no way to write raw strings except string literals (enforced
  with a macro that only accepts literals). SVG text goes through the same writer. Export
  downloads keep their own escaping (the CSV formula guard). JSON uses the canonical
  writer, served as `application/json` with `nosniff`.
- **Input limits**: request line 8 KiB, at most 30 headers (Mongoose's limit), body 256
  KiB (`MG_MAX_RECV_SIZE` lowered), form fields ≤ 200. Fields must be valid UTF-8, as in
  the CLI. Idle connections close after 30 s, with at most 64 connections.
- **`--read-only`**: no POST routes are registered and forms are not rendered.
- **No file system serving**: assets are compiled in and looked up by exact name.

### 13.7 Testing (M6 ✅)

- **Handler tests** (no sockets): fake `web_http` requests against core on `store_mem`.
  They cover golden HTML fragments, PRG redirects, 422 re-rendering, changed-fields-only
  writes and the stale-head conflict.
- **Security tests**:
  - 401 without a session; 403 on a bad or missing CSRF token or a foreign `Origin`;
    421 on a foreign `Host`; the CSP and the other headers on every route;
  - hostile titles, bodies, labels and comments (`<script>`, quotes, `javascript:` URLs,
    RTL overrides) escaped in every page and in the SVG;
  - a fuzz target for the form/query decoder.
- **Adapter tests** (`web.http`): a forked server on port 0 and raw sockets: keep-alive and
  pipelining, HEAD, 413/501/415/400 before the body is read, garbage, 200 headers, idle
  timeout, the connection cap.
- **Smoke test** (`web.smoke`): the real `tix-web` on port 0 in a throwaway git repository.
  It logs in, creates, edits and comments over real HTTP, while the CLI writes next to it
  (the next page must show it). The CLI then sees the web's writes. A forged form is
  refused. Then a soak of 3000 requests; under ASan, `tix-web` must exit 0 on SIGTERM, so
  a leak per request fails the test (mutation-checked).
- **Mutation checks**: the stale-tab test fails with conflict detection off; the store
  refresh test fails with refresh as a no-op.
- **Browser checks** (by hand in M6, headless Chrome): every page in light and dark with no
  CSP violation, and the forms driven with JavaScript disabled. This found the
  `Referrer-Policy` / `Origin: null` problem (§13.6).
- **No-JS check** (M7): every M7 interaction has a test proving its form fallback works.

### 13.8 Hosted mode later (not built in M6–M7)

- **Trusted-proxy auth**: `--trust-proxy` lets the server bind beyond loopback, and only
  then. It requires TLS at the reverse proxy and takes the user from a configured
  header (e.g. Authentik's `X-authentik-username`).
- **Identity mapping**: logins map to tix identities through a mapping file, and writes
  use the requesting user's identity instead of `tix.identity`. Edits are then
  attributable per user.
- **Multiple repos**: one server serving several repositories under `/r/<name>/`.

Everything in M6–M7 stays compatible with this. Handlers receive the identity and the repo
from the request context rather than from globals. The auth check is one function.

### 13.9 Milestones

- **M6 web (no JS)**:
  - the `input` module extracted from the CLI (no behaviour change, CLI tests unchanged);
  - store `refresh`;
  - `third_party/mongoose` and `apps/web` with the adapter, router, html writer, auth, CSRF
    and headers;
  - every page and form in §13.3 and the SVG Gantt;
  - `tix web`, the tests in §13.7, and docs.
- **M7 web JS**: `app.js`, `/api/tickets`, Gantt drag/resize/link, inline status,
  shortcuts and SSE live refresh.
