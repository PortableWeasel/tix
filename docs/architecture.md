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
refs/tix/baselines/<id>       plan snapshot (after v1, §14.2)
refs/tix-remotes/<remote>/…  fetched copies; merged into refs/tix/* by `tix pull`
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
   Manual tasks are fixed points. A computed start moves to the next working day; a fixed
   start (actual dates, a manual start) stays on the date given, even on a non-working day,
   because work recorded on a Saturday happened then (found dogfooding, v0.6). Summary tasks span min(child start) to max(child finish).
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
pull:  git fetch --prune -- <remote> +refs/tix/*:refs/tix-remotes/<remote>/*
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
push:  every valid local entity ref that differs from refs/tix-remotes/<remote>/…
       git push --porcelain -- <remote> <ref>:<ref>…     (never '+', never forced)
       then record the pushed values in refs/tix-remotes/<remote>/…
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
- The mirror lives in `refs/tix-remotes/<remote>/`, outside `refs/remotes/`. Until v0.6 it
  was `refs/remotes/<remote>/tix/`, where a remote branch literally named `tix` (which git's
  default refspec maps to `refs/remotes/<remote>/tix`) collided with it, a directory/file
  conflict that made one of the fetches fail. After a successful fetch, store_libgit deletes
  leftover old-layout refs, but only names shaped like `<kind>/<hex id>`, so a real
  remote-tracking branch such as `tix/feature` survives. Remote names are one ref component,
  so two remotes' mirrors can't nest. `git remote remove` doesn't know about the mirror; a
  removed remote's refs there are stale but harmless.
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
- **Ticket lists filter on summaries** (index port v2, schema 2). Profiling `ls` on 5,100
  tickets showed 75% of a warm run spent decoding cached entities with cJSON. Each cached
  ticket now also carries a summary: status, author, parent, rank, title, created, updated,
  max Lamport, labels and assignees, in a length-prefixed encoding that core owns (the
  index stores it as opaque bytes, like the entity). `tix_ticket_list` reads every summary
  with one `scan`, filters and sorts on them, and decodes only the tickets it returns.
  - A row counts only if its head equals the ref's current head and its summary decodes
    (the header carries `TIX_ENTITY_CODEC_VERSION`). Otherwise that ticket is loaded, as
    without a cache, and its entry is rewritten.
  - Matching stays in core rather than SQL `WHERE`: one implementation of the text match
    and the WBS rules, and WBS ordering and `--under` need every ticket's parent anyway.
  - Measured on 5,100 tickets, warm, release build: `ls --status done` 0.22 s → 0.06 s,
    `ls --label X --all` (1 in 7) → 0.12 s; `ls --all` unchanged (it decodes everything).
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
tix new   [-t title] [-l label]... [--status s] [--parent P] [--kind milestone]
tix ls    [--status s] [--label l] [--assignee r] [--under P] [--sort status|wbs|updated|newest]
          (default status: in-progress, blocked, open, done, cancelled, each in WBS order)
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
tix export [root] --format mermaid|csv|json|mspdi
tix pull | push | sync [remote]
tix --version                          tix and the linked git.git version
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
8. ✅ **M7 web UI enhancements** (done 2026-11-07, §13.4): search as you type, Gantt editing,
   live refresh.
9. **After v1** (approved 2026-11-07, §14): MS Project XML export ✅, plan baselines ✅, commit
   signature enforcement ✅, Forgejo issue bridge ✅, TUI ✅, hosted web mode ✅.

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

### 13.4 JavaScript enhancements (M7 ✅)

One hand-written `app.js` (no framework, no build step, no third-party code) that each
page loads with `defer`. Its source and the stylesheet live in `apps/web/assets/` and are
compiled in by `cmake/Embed.cmake`, served under content-hashed names. Progressive
enhancement only: every request it makes is one a form or link can also make, and nothing
it does is needed to use tix.

- **Search as you type**: `GET /api/tickets?q=&limit=&status=` returns
  `{"tickets":[{"id","short","title","status","stage"}],"more":bool}` from the canonical JSON
  writer (same matching as the list; active first, then WBS order; limit ≤ 50). The header
  search box and the `parent`, `pred` and `under` fields become ARIA comboboxes. Results
  are placed with `textContent` only. Without JS they are the search form and text
  fields taking an id prefix.
- **Gantt editing**: the SVG carries its geometry (`data-s0`, `-step`, `-colw`, `-x0`)
  and each bar its stored fields and head (`data-start`, `-finish`, `-dur`, `-mode`,
  `-head`, `-kind`). `data-edit` is present only when writes are allowed.
  - Drag a bar: `start` (manual tasks also `finish`) shifted by whole columns.
  - Drag its right end: auto tasks get `duration` (old duration plus the Mon–Fri days
    crossed; ±5 per week column), manual tasks `finish`. The server's calendar decides
    the real schedule, and the chart is re-drawn from it at once.
  - Drag the handle at a bar's end onto another bar: `POST /t/OTHER/dep` with an FS link.
  - A focused bar takes ←/→ (Shift: length), saved after a 0.7 s pause; Esc cancels.
  - Summary bars and tasks with actual dates (`data-kind="sum"|"fixed"`) don't drag.
  - Each edit posts the edit form's fields with their originals and `head`, so the
    stale-tab rule (§13.5) applies unchanged; a refusal shows the server's message.
- **Inline status**: the list's status cells become selects, and the ticket page's status
  form applies on change. Both post `status` / `o.status` / `head` to `POST /t/ID`.
- **Keyboard shortcuts**: `/` search, `g t|w|p|l|r|c` go to a page, `n` new, `e` edit,
  `j`/`k` and Enter in lists, `?` help. Ignored while typing in a field.
- **Live refresh**: `GET /events` is a `text/event-stream`. Each page carries
  `data-stamp`, a fingerprint of every `refs/tix/*` ref (`tix_repo_stamp`). The stream's
  first event is the current stamp. While a stream is open the server refreshes and
  re-stamps once a second, and sends an event when the stamp changes.
  - The page then re-reads itself and swaps `<main>`, keeping focus on the same ticket.
  - While the user is typing in a form it shows a "changed elsewhere" banner instead, so
    no input is lost.
  - The HTTP port gained a streaming response and a server tick for this (still nothing
    names Mongoose outside its adapter). Streams are exempt from the idle timeout, get a
    comment line after 15 s of silence, and are capped at 8 (more get 503; the browser
    then doesn't retry).
  - The stamp reveals only that something changed. It sits behind the same session check
    as every page.

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

### 13.7 Testing (M6 ✅, M7 ✅)

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
- **M7 tests**:
  - handler tests for `/api/tickets` (JSON escaping of hostile titles, limits, the
    session check) and `/events` (stream flag, first stamp equals the page's, ticks speak
    only on change);
  - adapter tests for streams (no Content-Length, tick events, pings, idle exemption, the
    cap and a freed slot);
  - the smoke test opens a real event stream and sees a CLI write arrive;
  - `web.browser` (`apps/web/tests/browser.py`, headless Chrome via Playwright under
    `uv`; skipped when missing) drives every enhancement and reads the result back with
    the CLI: search and Enter, hostile titles shown as text, inline status on both pages,
    drag-move, drag-resize, drag-link, arrow keys with focus kept, live refresh in place,
    the typing banner, shortcuts, the dependency picker; plus no CSP violation, console
    error or failed request. (Playwright's `wait_for_function` uses `eval`, which the CSP
    rightly refuses, so the test polls instead.)
  - The no-JS paths are the M6 tests, unchanged: the forms JS posts are the same forms.

### 13.8 Hosted mode ✅ (after v1, §14.6)

Built as planned, with one change: libgit allows one repository per process, so "several
repositories under `/r/<name>/`" became one `tix-web` per repository, each on its own host
name behind the proxy. Details in §14.6.

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

## 14. After v1 (approved 2026-11-07)

Each item lands as its own commit series with tests, recorded in tix under
`tix: after v1` (5484c09).

### 14.1 MS Project XML export ✅

`tix export --format mspdi` (and the plan page's "MS Project" link) writes MSPDI, the XML
that Microsoft Project, ProjectLibre, GanttProject and MPXJ read. It lives in the `export`
module (`src/mspdi.c`), pure like the other renderers. `tix_schedule` now also carries the
plan's calendars (sorted by id, like resources).

- **Order**: MSPDI readers are order-sensitive (schema sequences). The element order follows
  MPXJ's generated schema classes; the export test checks every `<Task>` against it.
- **Time**: durations are working days × 8 h (`MinutesPerDay` 480), lags and total slack in
  tenths of minutes. Days run 08:00–12:00 and 13:00 onwards for the calendar's hours, so a
  6-hour day ends at 15:00. A milestone's start and finish are both the day's end.
- **Calendars**: UID 1 is the built-in Mon–Fri 8 h ("Standard"); tix calendars follow in
  id order. tix exceptions may overlap (the later op wins); MSPDI's may not. They are
  flattened into runs of covered days that share the resulting working/off status and the
  winning note.
- **Tasks**: UID 0 is the project summary; then each task in WBS order (`WBS` and
  `OutlineNumber` = the tix WBS code). Auto tasks with a start get "start no earlier than";
  manual tasks get `Manual` with fixed start and finish. Deadlines, progress, actual dates,
  notes (the body) and FS/SS/FF/SF links with lags carry over. Unscheduled tasks are
  `Estimated` with no dates. Cancelled tasks are inactive.
- **Resources and assignments**: every resource is a work resource with `MaxUnits` from
  capacity and an hourly `StandardRate`. A currency is written when all rated resources
  share one. Assignments carry units and, where set, effort as `Work`.
- **Escaping**: text is XML-escaped; control characters XML 1.0 forbids become spaces.
- **Verified** by reading exports back with MPXJ 13 (via its Python package and a JRE in a
  scratch directory): calendars and exceptions, outline, constraints, deadline, notes,
  links with lags, the manual task, the milestone, actuals, rates, units and work all came
  back as written. 5,100 tasks export in 2 s (debug build) and read back whole.

### 14.2 Plan baselines ✅

A baseline is a snapshot of the computed plan, stored as a fifth entity type,
`refs/tix/baselines/<id>`, so it syncs, merges and validates like everything else. Older
clients see an unknown kind and ignore it (§6).

- **Ops**: `create` {nonce, name, taken (date), root (id or null = whole plan)},
  `set-name`, and one `task` op per planned task {ticket, start, finish, duration, effort,
  cost, currency}; `null` means unknown or unscheduled. Dates come in pairs with
  start ≤ finish; a cost needs a currency and vice versa. For a ticket, the last `task` op
  in total order wins, so a correction is just another op. Nothing else changes after
  creation.
- **Taking one** (`tix_baseline_take`): core plans from the root as of today and writes the
  ops as a chain of packs (≤ `TIX_PACK_MAX_OPS` each), then moves the ref once, so a
  5,000-task plan is still one atomic snapshot.
- **Cost** (new in `plan`): per task, Σ assignment hours × the resource's hourly rate, in
  hundredths of the currency. Hours are the assignment's effort, else its share of the
  task's effort (by units × capacity), else duration × calendar hours × units × capacity.
  Unrated resources cost nothing. Two currencies on one task, or under one summary, make the
  cost unknown rather than a meaningless sum. Summaries add their children.
- **Comparing** (`tix_plan_compare`, pure): per task, whether it was baselined, start and
  finish slip in the task's working days (positive = later; Fri→Mon is 1), and duration,
  effort and cost deltas when both sides are known (cost only in the same currency); plus
  how many baselined tasks have left the plan.
- **Frontends**:
  - CLI: `tix baseline take NAME [ROOT] | ls | show B | rename B NAME`, and `--baseline B` on
    `wbs`, `plan` (BASE FIN and SLIP columns, a summary line) and `export`.
  - Exports: CSV gains `cost,currency` always and seven variance columns with a baseline
    (money as decimals); JSON gains `cost`/`currency` and a `baseline` object per task
    (money in hundredths); MSPDI gains `<Cost>` and a `<Baseline>` (Number 0) per task.
  - Web: `/baselines` lists them, takes one and renames; the plan page has a baseline
    picker, "Baseline finish" and "Slip" columns, and draws the baseline as a thin bar under
    each Gantt bar; export links carry the baseline.
- `TIX_ENTITY_CODEC_VERSION` is 2 (the cache learns the new type).
- **Verified**: model round trips and rejects, last-wins and convergence on two clones,
  the cache codec, a 1,100-task chain under one ref in core, golden cost and slip cases in
  plan, the CLI end to end including a clone that pulls the baselines, web handler tests
  with a hostile name, and MPXJ reading the MSPDI baseline and cost fields back.

### 14.3 Commit signatures ✅

Identities carry ssh and gpg keys (`key-add`, §4.1). tix now signs op-pack commits and
checks incoming ones against the keys of the identity each commit is authored by.

- **Signing**: `git config tix.sign true` makes store_libgit sign every commit it writes
  with git's own machinery (`user.signingkey`, `gpg.format` ssh or openpgp, so ssh-agent or
  gpg-agent work as for `git commit -S`). `tix id edit --add-signing-key` adds that key
  to your identity: the public half (ssh: `type base64`; gpg: an armored export) and its
  fingerprint (`ssh-keygen -l -E sha256`, `gpg --with-colons`).
- **Verifying** (store port `commit_verify`): the adapter extracts the signature and the
  signed payload (`parse_signed_commit`) and runs the verifier itself, as child processes
  with argv only, against keys passed in, never the user's keyring:
  - ssh: `ssh-keygen -Y verify -n git` with a one-line allowed-signers file per candidate key;
  - gpg: a throwaway `--homedir` with the identity's keys imported, `--status-fd`, and
    the `VALIDSIG` fingerprint compared with the key's (full, or its long key id).
  - Result: NONE (unsigned), GOOD (and which key), BAD (invalid, or by no given key,
    including a key of the wrong kind). store_mem simulates signatures for core tests.
- **Whose keys**: the identity named as the pack's author, as it is locally. For an
  identity's own history (whose root has no author), the incoming version of that identity,
  so an identity can sign its root with a key it adds in a later op. Pull integrates
  identities before everything else, so key changes arrive before the commits they sign.
  Key removal is not retroactive: old commits are then checked against the current keys.
- **Policy** (`git config tix.verify` off|warn|require, default off; `tix pull/sync
  --verify P` for one run): only commits new to this clone are checked (trust on first
  use; history already here is never re-judged).
  - warn: count, report and adopt.
  - require: an unsigned or badly signed new commit rejects the whole incoming ref (the
    same path as an invalid one: reported, never adopted, never pushed on).
  - A new clone of a repository with unsigned history pulls once with `--verify warn`.
    Pack times and Lamport values are author-controlled, so no date cut-off is offered:
    it would be forgeable.
- **Audit**: `tix verify [ID]...` checks whole local histories and prints per-entity
  counts for anything unsigned or bad, plus a total. It exits 1 on a bad signature, or on
  unsigned commits under require. The web sync page shows the incoming counts.
- **Tested**: the store contract (unsigned → NONE, missing → NOT_FOUND);
  store_libgit with real ed25519 ssh keys and real gpg keys in a throwaway home (good, the
  right key among several, another key, no keys, the wrong kind, unsigned, and a tampered
  copy of a signed commit → BAD); core on store_mem (an identity signing its own root with
  a later key, require rejecting unsigned and foreign-key commits as a whole ref, warn
  adopting with a note, the audit; disabling the check fails it); and cli.sh with real ssh
  keys end to end, including the trust-on-first-use pull.

### 14.4 Forgejo issue bridge ✅

`tix bridge pull|push|sync` keeps tickets and a Forgejo (or Gitea) repository's issues in
step: title, body, open/closed, labels, and new comments both ways.

- **Layers**: module `bridge` holds the port (`tix_tracker`: list, labels, create, update,
  comments, comment) and the algorithm; `bridge_forgejo` is the adapter (REST API v1 over
  libcurl, JSON with cJSON) and the only module that includes `curl.h` (enforced at
  configure time by `cmake/Quarantine.cmake`, which now also guards `sqlite3.h`).
- **Links**: a new ticket field `external` ("forgejo:host/owner/repo#12", LWW like the other
  fields; codec version 3) syncs with tix, so every clone knows which ticket is which issue.
  Issues tix creates end with an invisible `<!-- tix:<ticket id> -->` marker: a link lost
  between creating the issue and writing the field is found again on the next run. Two
  tickets on one issue, a marker naming a ticket that links elsewhere or isn't here yet, and
  a link to an issue that's gone are reported as conflicts, never guessed at.
- **Changes**: each clone keeps a base (local state `bridge-<hash of the tracker key>`: per
  issue, a hash of each field as both sides last agreed). A field changed on one side is
  copied to the other. Changed on both sides, or different and never bridged from this
  clone, it is a conflict: reported, nothing overwritten, and it clears once the sides
  agree. `pull` only copies issue → ticket and `push` only ticket → issue; a change waiting
  for the other direction stays pending.
  - Status maps to open/closed: done and cancelled are closed; an issue closed elsewhere
    makes the ticket done, reopened makes it open.
  - Labels: only names the tracker has and tix accepts as labels are bridged (others stay
    where they are, so nothing is copied back and forth forever). Labels are never created
    in the tracker.
- **Scope**: every linked ticket syncs. `push` creates issues only for unlinked tickets in an
  explicit scope: `--under ID`, `--label L` or `--all`. `pull` imports every unlinked issue
  without a marker.
- **Comments**: tracker → tix comments start with "> author wrote on <key>#n (comment id):";
  tix → tracker comments end with a marker carrying the tix comment's op id. Each side's
  markers are what de-duplicates, so two clones bridging the same issue never echo each
  other's comments (no base needed). Comment edits are not copied (v1).
- **Remote text is hostile**: titles and bodies are made valid UTF-8, CR dropped, control
  characters replaced, titles single-line, both cut to tix's limits; labels tix can't hold
  are skipped.
- **Adapter safety**: TLS verified; plain http only for loopback; redirects are never
  followed (the token goes to that host only); responses capped at 32 MiB and 120 s;
  pull requests and malformed entries are skipped; server messages are shown without
  control characters. The token comes from `TIX_FORGEJO_TOKEN` or git's credential helpers
  (`git credential fill`, run as a child with `GIT_TERMINAL_PROMPT=0`); it is wiped from
  memory after use and never printed.
- **Shown**: `tix show` prints `issue:`; the web ticket page links to the issue, but only when
  the synced value has exactly the shape forgejo:host/owner/repo#n in safe characters.
- **Tested**: the algorithm against an in-memory tracker (dry run, import with hostile titles
  and labels, both directions, pull-only and push-only, conflicts and their clearing, state,
  scoped export with marker and label filtering, a base-less clone without echoes, relinking
  by marker, duplicate and dangling links, marker parsing); the adapter against a fake
  Forgejo in Python (auth header, pagination with a pull request and a malformed entry,
  label ids, marker kept on edit, 404 with an escape sequence in the message, a redirect
  not followed, a non-JSON reply, URL parsing refusing plain http, extra path, `..` and
  userinfo); and a real Forgejo 10 (a throwaway container from the production image on a
  loopback port, removed afterwards): import with comment, title push, export with marker
  and label, comment out, close and edit flowing back, an idle second run, and a conflict
  that overwrote nothing. (The tokens in pass lack `write:issue`, so the production
  instance wasn't used.)

### 14.5 Terminal UI ✅

`tix tui` (the binary `tix-tui`, exec'd like `tix-web`) browses and edits in the terminal:
the ticket list with a live id/title filter, one ticket (fields, planning, body, comments),
the WBS, and a Gantt chart. A composition root like the CLI: lists come from
`tix_ticket_list`/`tix_plan`, every edit is an op through `tix_entity_apply`, and the
parent goes through `tix/input.h` exactly as `tix edit --parent` does. No rule lives here.

- **Keys**: `1`/`2`/`3` (or `t`/`w`/`g`) switch views; `j`/`k`, arrows, PgUp/PgDn, Home/End
  move; Enter opens, Esc/`q` goes back; `/` filters as you type, `a` toggles done
  tickets; `s` status, `e` title, `l` labels (+add -remove), `c` a one-line comment, `C` a
  comment and `E` the body in `$EDITOR`, `n` a new ticket (a subtask from inside a
  ticket), `p` parent; `h`/`l` scroll the Gantt by a week; `?` help.
- **Live**: every second without a key, it refreshes and compares `tix_repo_stamp`, so
  changes from the CLI, the web UI or a sync appear by themselves.
- **Terminal safety**: curses lives only in `src/term_curses.c` (configure-time
  quarantine). Everything drawn passes through it: invalid UTF-8, C0/C1 controls, line
  separators and bidi overrides become '?', so text synced from other clones can't move
  the cursor, retitle the terminal or reorder what you read. Text is clipped and wrapped
  by display width (wide characters count two).
- **`$EDITOR`**: the round trip moved to `apps/common` (shared with the CLI); curses is
  suspended around it.
- `typeahead(-1)`: ncurses abandons a redraw while input is pending, which left half-drawn
  screens when keys came in a burst (a paste, fast typing); found by the pty test.
- **Tested** (`tui.pty`): the real binary in a pseudo-terminal, read back through a
  terminal emulator (pexpect + pyte under uv), every edit checked with the CLI: list,
  live filter, open, status, comment, title, labels, body via `$EDITOR` (a `sed` command),
  done tickets, new ticket and subtask, WBS, Gantt, help, a CLI change appearing by
  itself, a bidi override drawn as '?', clean quit. The test uses `TERM=vt220` because
  pyte doesn't implement `CSI n S/T`, which ncurses uses for scrolling with xterm. Also
  found and fixed in `plan`: a summary whose children are all unscheduled had dates of day
  0 (1970-01-01) instead of none (golden case added).

### 14.6 Hosted web mode behind SSO ✅

`tix web --trust-proxy HEADER --public-url https://tix.example.org [--proxy-from ADDR]...
[--users FILE] [--logout-url URL]` serves the web UI behind a reverse proxy that signs
people in (Authentik's proxy outpost sends `X-authentik-username`).

- **Who is asking**: the login comes from the configured header, and only on connections
  from the proxy's addresses (`--proxy-from`, default 127.0.0.1 and ::1). Anything else
  gets 403, a missing or unusable login 401. The HTTP port carries the peer address and
  one extra header for this, still without naming the library.
- **Where**: `Host` must be the public URL's host (421 otherwise), and POSTs need that
  exact `Origin` (or `Referer`) plus the session's form token, as in local mode. There is
  no login token and no cookie: one session per login, made on first sight (least recently
  used dropped at 64), holds the form token. `/login` and `/logout` don't exist; the
  page shows the login and the proxy's sign-out link.
- **As whom**: `--users` lists `LOGIN IDENTITY-ID` lines (resolved at start; a bad line
  stops the server). Every request sets the repository's identity to the requester's, or
  clears it, so a write is authored by the person who made it and nothing carries over
  between requests (single-threaded, rule 8). Logins not in the file can read, see no
  forms (`web_can_write`), and get 403 on any POST.
- `--listen` takes any address in this mode (e.g. `0.0.0.0:7480` for a proxy in a
  container network); outside it, loopback only, as before.
- **Several repositories**: one `tix-web` per repository, each on its own host name behind
  the proxy (libgit has process-wide state, §7). A path prefix would have meant
  rewriting every link for little gain.
- **Not deployed**: putting a repository on the internet behind Authentik (DNS, proxy
  provider, firewall) is an operations decision left to the owner. The mode is built and
  tested, ready for it.
- **Tested**: handler tests (only from the proxy, only under its host, a login with a
  control character refused, no token login; Ada's and Bob's writes authored by Ada and
  Bob; another login's token and a foreign origin refused; Eve reads without forms and is
  refused on POST; no identity left behind after a request; disabling the peer check or
  the per-request identity fails them), the real `tix-web` over HTTP with a users file
  (401 without a login, a write by ada authored by her identity, 403 for an unmapped
  login), and the CLI's flag rules.


## 15. Agent workflow (approved 2026-11-07)

Goal: let a person and AI agents hand tickets back and forth through tix: the person writes
a ticket, an agent refines it, asks questions or declares it ready, another agent
implements it, a third reviews it, and the person accepts the result. Agents may also run
unattended overnight without any ticket stalling on a human.

**Where the line is.** tix provides *mechanisms*; the workflow itself is *policy* and lives
outside tix. Something belongs in tix only if (a) correctness needs atomicity, merge
semantics or a view every clone shares, or (b) a team of only humans would also use it.
Everything else is either:

- **the skill**: agent instructions (`docs/agent-workflow.md`, mirrored as the Hermes skill).
  It covers who may move a ticket where, the review loop, round counting, escalation,
  assumptions, aborts and workarounds. It can change without a tix release.
- **the runner**: whatever launches agents (Hermes cron, a loop script). It owns time budgets
  and the circuit breaker.

Recorded in tix under `tix: agent workflow`. Nothing here changes the planning engine:
every new piece is either orthogonal to it or sits below the five phases it already
understands.

### 15.1 Status = phase / stage ✅

`status` keeps its five fixed **phases** (`open`, `in-progress`, `blocked`, `done`,
`cancelled`). Each may carry an optional free-form **stage**, written `phase/stage`
(`open/ready`, `in-progress/review`). The planning engine, `started_at`/`done_at`, the
Gantt and every existing filter see only the phase. `--status open` matches every open
stage, and `--status open/ready` matches one stage.

- **Stored as one register.** The phase and stage are one LWW value, so two concurrent
  edits can never merge into a pair nobody wrote (`done/refine`). New op
  `{"op":"set-state","status":"open","stage":"ready"}` (stage `null` = none). For older
  readers, a writer emits `set-status` with the same phase on the line *before* each
  `set-state`. Older readers skip the unknown op and still see the right phase. New
  readers treat a `set-status` followed directly by a matching `set-state` as one write,
  and a lone `set-status` as "phase, stage cleared".
- **Stage names**: `[a-z][a-z0-9-]{0,31}`.
- **The project's stage list** is a field on a WBS root, inherited by its subtree (like
  `calendar`): `stages`, an OR-map `name → {phase, human: bool, rank}`. The CLI is
  `tix stage ls [ID]` (the list that applies to a ticket, and where it comes from; without
  an id, every ticket that has one), `tix stage add ID NAME --phase P [--human] [--rank N]`
  (rank defaults to 10 past the phase's last stage) and `tix stage rm ID NAME`.
  - `human` marks a stage that waits for a person. The inbox (§15.6) and the guard (§15.5)
    use it.
  - Core refuses a stage that isn't on the effective list, or that is listed under another
    phase, to catch typos. With an empty list, any stage is accepted.
  - Merge accepts anything. A stage missing from the list is shown as unlisted, never
    rejected.
- **tix ships no stage list.** The agent workflow's list lives in the skill
  (`docs/agent-workflow.md`), which also explains how to install it. For reference:

  | Phase | Stages (`*` = human) |
  |---|---|
  | open | `draft`\*, `refine`, `needs-input`\*, `ready` |
  | in-progress | `implementing`, `review`, `changes-requested`, `needs-input`\* |
  | blocked | `aborted`\* |
  | done | `unaccepted`\*, `accepted` |

  Two consequences of this list:
  - A dependent is satisfied when its predecessor's *phase* is `done`, whatever the stage.
    So `done/unaccepted` lets dependents start, and nothing special happens in
    autonomous mode.
  - `blocked/aborted` is unsatisfied, like every `blocked`.

- **Core** adds the `set-status` line itself when a caller writes a lone `set-state`, so
  frontends only push `set-state` (or `set-status` for a bare phase). The batch's own
  `set-parent` and stage-list ops count when checking a stage, so `tix new --parent P
  --status open/ready` checks against P's list.
- **Index**: the stage is part of the cached ticket summary that lists filter on
  (`tix_ticket_query.stage`); `TIX_ENTITY_CODEC_VERSION` is 4.
- **Frontends**: CLI `--status PHASE/STAGE` on `new`, `edit` and `ls` (one stage per
  `ls`), `show` prints the state and the ticket's own list; the web UI's status selects
  offer each phase followed by its listed stages (an unlisted current stage stays
  selectable), pages show `phase/stage`, and `/api/tickets` and `export --format json`
  carry `"stage"`. The TUI shows it; its `s` key still sets bare phases.

### 15.2 Identity kind ✅

Identities gain `kind`: `human` (default) or `agent`. It's an LWW field, protected like the
rest of an identity (only the identity itself may write it). The kind is what the guard
(§15.5) and the UI's badges go by.

It is self-declared, so it prevents accidents, not malice. Commit signatures (§14.3) prove
who wrote an op, but not what kind of author they are.

Stored as the identity scalar `set-kind` (`{"op":"set-kind","kind":"agent"}`, the field
table's `TIX_F_ID_KIND`; older readers skip it). CLI: `tix id new --kind agent`, `tix id
edit --kind human|agent`, shown by `id show` and marked in `id ls`; the web UI puts an
"agent" badge after an agent's name, the TUI "(agent)".

### 15.3 Claims ✅

A claim says "this identity is working on the ticket until T". It's generic: a person can
claim a ticket too.

- **Fields**: `claim` = `{identity, since, until}` or empty, and `waiting`, the claims queued
  behind it. Ops are `{"op":"claim","until":<unix>}` (the claimant is the pack author) and
  `{"op":"release"}`.
- **Replay** (not LWW), in total order:
  - `claim` by A at pack time T: A holds the ticket if it is free, A's own (a renewal,
    which keeps `since`) or expired at T. Otherwise A **waits**, queued in arrival order
    (a renewal updates its `until`).
  - `release` by A at T: drops A's waiting claim. If A holds the ticket, or the hold
    expired at T, it is freed and the first waiting claim still live at T takes over.
    Someone else's live claim can't be released; it ends with its lease.

  A claim therefore never takes over a live one, and concurrent claims resolve the same
  way on every clone.
- **Why a queue** (found while testing, 2026-11-07): merge order is the Lamport order, not
  the order claims reached the remote. If losing claims were simply dropped, a claim pushed
  *later* but ordered *earlier* would overturn one an agent had already been told it won.
  With the queue, the side that lost the race can hand the ticket over by releasing.
- **Whoever sees a contested claim yields.** The claim that reached the remote first was
  pushed without seeing the other, so its owner never sees a contest. The other side sees
  both claims as soon as it pulls, and releases in the same sync that publishes its claim.
  `claim --next` does this after every pull. Core's write check (a claim over someone's
  live claim is refused) means waiting claims only ever come from such races.
- **Time has a meaning here.** This is the first rule that gives a pack's `time` meaning
  beyond display (§3.3). Clock skew between agents moves a lease edge by the skew, which is
  acceptable for leases of minutes. On write, `until` must be after now and at most now +
  24 h.
- **Liveness for display and queues** compares `until` with a "now" that the caller passes
  in (rule 7). The web app's clock is `web_app_config.now`.
- **CLI**:
  - `tix claim ID [--for 30m]` (also renews), `tix claim ID --release`
  - `tix claim --next [--status S]... [--under ID] [--for 30m] [--remote R | --local]`.
    The remote defaults to `tix.remote`, else `origin`, and is used only when configured.
  - `show` prints the claim and anyone waiting; `ls` adds `@name` to live claims.
- **`claim --next`** (`tix_claim_next`) is the agent's one call to get work:
  1. Pull from the remote, if any, then yield any of my claims that are contested.
  2. Candidates in WBS order: in a requested state (default: the open and in-progress
     phases), not claimed live by anyone else, every predecessor done or cancelled, not in
     a `human` stage. A ticket I already hold comes first and is renewed.
  3. Write the claim and push. If the push is refused, pull. If the merge shows someone
     else's live claim on the ticket, release mine, sync, and try the next candidate (up
     to 5); otherwise push again.
  4. Print the claimed ticket, or exit 3 for "no work".

  Without a remote, the CAS ref update and the write check already serialise claims
  between agents sharing a repository. The autonomy halt (§15.5) adds one more filter.
- **Tested**: replay of renewals, expiry, holder-only release and hand-over in both
  orders; convergence with random claims and releases; core races where the loser's
  fetch was stale (refused push) and where its own claim was still unpushed; a `cli.sh`
  race between two clones over a bare remote.

### 15.4 Typed comments ✅

New op
`{"op":"typed-comment","kind":"review","body":"…","data":"{…}","reply-to":"<op id>"|null}`.

- `kind` follows the stage-name rule.
- `body` is markdown, as in a plain comment. It is what older readers and plain views show.
- `data` is a JSON object carried **as a string**, which keeps the canonical codec simple.
  Its size is capped at 64 KiB together with `body`. Core validates that it is an object
  of the generic shape below.

tix interprets `data` only through generic shapes, never through the kinds:

```
{"fields":{"verdict":"changes","commit":"abc1234"},     key/value table
 "items":[{"id":"f1","text":"…","state":"open"}],       checklist (ids unique per comment)
 "questions":[{"id":"q1","text":"…"}],                  rendered as an answer form
 "updates":[{"ref":"<op id>#f1","state":"addressed"}]}  folds into earlier items
```

- Answering a question form posts a reply (`kind: answer`, `reply-to` set) whose `fields`
  map question ids to answers.
- An item's current state is its latest `updates` entry in total order.
- `question`, `answer`, `review`, `assumption`, `abort` and `workaround` are just the
  skill's kind names. The UI shows a kind as a badge, plus `fields` / `items` /
  `questions`.
- Older readers ignore the op.
- Editing a typed comment follows the `edit-comment` rule (author only, LWW).
- **Validated in `model`** (`tix_note_data_check`), on write and on read like every other
  field: one JSON object; `fields` values are strings (names `[a-z0-9][a-z0-9-]`), item and
  question ids `[a-z0-9][a-z0-9-]{0,31}` and unique per comment, states follow the
  stage-name rule, `updates` refs are `<op id>#<item id>`, texts ≤ 4 KiB, lists ≤ 256
  entries; other top-level keys are kept and ignored. A malformed one rejects its pack.
- `tix_note_item_state` folds updates (anyone may update an item). Core checks that a
  `reply-to` names a comment on the same ticket.
- **CLI**: `tix note ID --kind K [-m TEXT] [--reply-to CID]` with `--data JSON`,
  `--data-file F|-`, or shortcuts that build the JSON: `--field K=V`, `--item
  ID=TEXT[=STATE]` (state defaults to `open`), `--question ID=TEXT`, `--update
  CID#ITEM=STATE` (short comment ids are expanded). `show` prints the kind, the reply, and
  the data with each item's current state. Entity codec v7.

### 15.5 Autonomy and the human-stage guard ✅

- **Field**: `autonomy` on a WBS root, inherited by its subtree: `{mode, until}` with mode
  `supervised` | `autonomous` | `halted`, LWW.
  - `autonomous` with `until` in the past counts as `supervised`, so a forgotten switch
    expires by itself.
  - `halted` stops `claim --next` for the subtree until a human changes it.
- **Who may set it**: humans may set any mode. Agents may only set `halted`, so the runner's
  circuit breaker can stop a night but never start or resume one. This is checked in core,
  by the identity's kind.
- **The guard**: while the effective mode is `autonomous`, core refuses
  (`TIX_E_REFUSED`) any write by an `agent` identity that would put a ticket into a stage
  tagged `human`. The message tells the agent to decide and record an assumption, or abort,
  instead.
  - The check is local. Merges never reject ops; incoming data is validated as usual.
  - It guarantees that a well-meaning agent can't park the night's work on a person.
  - **The `blocked` phase is exempt.** A blocked ticket never holds up the queue:
    `claim --next` skips only its dependents and keeps handing out other work. That makes
    `blocked/aborted` the agent's escape hatch for impossible tickets. It's a stronger
    "needs a person" that stops one branch of the plan, never the whole night.
- **Op**: `{"op":"set-autonomy","mode":"autonomous","until":<unix>|null}`, one LWW register
  (mode and expiry never mix across writes). `tix_ticket_autonomy` gives the effective
  setting for a ticket (its own, else the nearest ancestor's), with an expired autonomous
  reading as supervised. Policy refusals use the new `TIX_ERR_REFUSED`.
- **CLI**: `tix autonomy ID on --until 08:00|"YYYY-MM-DD HH:MM"|--for 10h|--no-expiry`
  (local time; an end is required unless `--no-expiry` says otherwise), `tix autonomy ID
  off|halt|show`; `show` on a ticket prints its own setting.
- The web header shows the mode, its expiry, and counts since it was switched on.

### 15.6 Agent and live interfaces

- **`--json`** on `ls`, `show`, `claim`, `stage ls`, `autonomy show` and `log`. Output comes
  from the canonical JSON writer, with field names matching the op schema.
- **`tix log [ID] [--since 2h|DATE] [--under R] [--limit N] [--json]`** ✅: the activity feed. It lists ops in total
  order with author, kind, ticket and summary. The web's activity page uses the same feed.
- **`tix watch [--under R] [--pull 30s] [--max-events N] [--json]`**: one NDJSON event per
  changed ticket (`{"ticket","short","title","head","ops":[…]}`, the ops being the new
  commits' activity entries). It compares ref heads once a second and pulls on the given
  interval. Agents use it to wait without polling `ls`; `--max-events 1` returns after the
  next change. ✅
- CLI shapes ✅: `ls --json` and `claim --next --json` print `{"id","short","title",
  "status","stage","labels","parent","claim":{"identity","name","until","live"}|null,
  "updated"}` per line (`claim --next` prints `null` with exit 3 when there's no work);
  `show --json` prints the canonical snapshot (every replayed field); `stage ls ID --json`
  `{"owner","stages":[…]}`; `autonomy show --json` `{"mode","until","owner"}`; `log`
  entries are `{"time","id","ticket","short","title","author","name","op","summary"}`
  (core `tix_activity_log`).
- **Web UI** ✅:
  - **`/inbox`**: "Waiting for you" (tickets in a `human` stage, and tickets with questions
    no later comment replies to), then "Being worked on" (live claims: who, for how long,
    how much lease is left).
  - **`/activity?since=2h|24h|7d`**: by default since autonomy was last switched on (else
    the last day). A summary line (changes, how many went to done and to blocked, typed
    comments by kind), then the feed, newest first.
  - **Typed comments** show a kind badge and what they reply to, then their `fields` (a
    table), `items` (with their current state), `questions` and `updates`. Questions come
    with an answer form (`POST /t/ID/answer`: `reply-to` plus `a.<question id>`). It writes
    an `answer` comment whose `fields` hold the non-empty answers, a plain form without
    JavaScript.
  - Claims: `@name` in ticket lists, "claimed by …, N min left" (plus anyone waiting) on
    the ticket page. Agents get an "agent" badge after their name.
  - **Autonomy banner** in every page's header for each root that is autonomous (not yet
    expired) or halted, linking to the activity page and the inbox.
  - **`/events`** sends `event: tickets` (`{"changed":[{"id","title","human"}]}`, up to 50)
    before each stamp. A ticket page (`<body data-ticket>`) refreshes only when its own
    ticket, or one it links to, changed; otherwise it just takes the new stamp. With
    permission (a button on the inbox), `app.js` shows a browser notification when a
    ticket starts waiting for a person.
- **Seeing other clones' pushes** ✅: the web server only sees its own refs.
  - `tix web --pull-every 60s` (10 s to 24 h) pulls on the server tick, which then runs
    even with no page open.
  - `POST /hooks/forgejo` exists only with `--hook-secret-file F` (16–255 bytes). It takes
    an `application/json` body (the only non-form body the decoder accepts, and only on
    that path) and checks `X-Forgejo-Signature` (or `X-Gitea-Signature`), an HMAC-SHA256
    of the body (new `tix/base_sha256.h`, RFC 4231 vectors). It needs no session or form
    token, because the signature is the check. A good signature pulls at once (204, or 502
    if the pull fails); anything else is 403. In hosted mode the proxy must let that path
    through.

### 15.7 Not in tix

Review rounds are counted from `review` comments by the skill. These all live in the skill:

- the stage transitions each role may make,
- not reviewing your own work, and giving the previous implementer first refusal,
- the escalation limit (`maxReviewRounds`, default 3),
- the shapes of assumption, abort and workaround notes.

Time budgets per ticket, the abort-rate circuit breaker and how many agents run belong to
the runner.

### 15.8 Testing

- **Convergence tests** for `set-state` (including with older readers), first-live-claim
  (every merge order of concurrent claims, renewals, expiries and releases), typed-comment
  updates, and inheritance of `stages` and `autonomy`.
- **Contract and handler tests** for the guard (an agent refused in autonomous mode,
  allowed in supervised and expired modes; a human always allowed), `claim --next` racing
  between two clones over a real remote, and the web inbox and answer forms with hostile
  content.
- Extend `cli.sh`, `browser.py` and the TUI pty test for every new command, page and key.
