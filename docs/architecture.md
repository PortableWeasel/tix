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
| `store` | base | `tix_store` vtable: op-pack commit write/read (the adapter owns the one-blob tree layout), ref read / list-by-prefix / compare-and-swap (CAS) update. Fetch/push are added in M3 |
| `index` | base, model | `tix_index` vtable: snapshot cache keyed by (entity id, head oid), queries |
| `core` | base, model, plan, store, index | The public API frontends use |
| `export` | base, model, plan | Render a computed plan to Mermaid gantt, CSV, JSON. No I/O: writes into a `tix_buf` |
| `store_libgit` | base, store, **libgit.a** | The **only** module allowed git.git headers |
| `store_mem` | base, store | In-memory store for fast, deterministic model/core/plan tests; content-addressed with a non-cryptographic hash; simulated fetch |
| `testkit`, `sim` | (tests only) | Check macros + PRNG; simulated clones (store_mem + clock + identity) that write, fetch and merge like core/sync will |
| `index_sqlite` | base, model, index, sqlite3 | `.git/tix/index.db`, disposable and rebuildable |
| `apps/cli` | core, export + adapter ctors | Arg parsing, formatting, ASCII Gantt/tree, `$EDITOR`, exit codes. **No business logic** |

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
{"op":"dep-add","pred":"c02e9f…","kind":"FS","lag":"2d"}
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

### 4.2 Ticket — `refs/tix/tickets/<id>`

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
| deps | key `pred id` → `{kind: FS\|SS\|FF\|SF, lag: ±Nd}` | OR-map | stored on the successor |

**Resource assignments**

| Field | Type | Merge |
|---|---|---|
| assignments | key `resource id` → `{units: % of capacity, effort: hours\|null}` | OR-map |

### 4.3 Resource — `refs/tix/resources/<id>`

| Field | Type | Merge | Notes |
|---|---|---|---|
| name | string | LWW | |
| kind | `person` \| `role` \| `equipment` | LWW | `role` = a generic pool ("2 × backend dev") |
| capacity | units (1.0 = one full-time unit) | LWW | a role can be 3.0 |
| calendar | calendar id \| null | LWW | own working time / leave |
| identity | identity id \| null | LWW | links a person resource to a user ("assigned to me") |
| rate | `{amount, currency, per: hour}` \| null | LWW | optional cost estimate |
| active | bool | LWW | archive without deleting |

✅ v1 has no edit control on resources or calendars (same as tickets): any author may edit.

### 4.4 Calendar — `refs/tix/calendars/<id>`

| Field | Type | Merge |
|---|---|---|
| name | string | LWW |
| weekdays | working-day mask (Mon–Sun) | LWW |
| hours_per_day | number | LWW |
| exceptions | `{from, to, working: bool, note}` (holidays, leave, extra days) | OR-set |

### 4.5 Cross-entity references

References (parent, pred, resource, calendar, identity) are just ids. A reference to an
entity that hasn't been fetched yet is kept and shown as *unresolved*; it is never an error
at write or merge time. Integrity problems (cycles, dangling refs) are handled at **read time**
in `plan`, deterministically (§5.2).

## 5. Planning engine (`plan`, pure)

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
   calendar hours/day)`, rounded up to whole working days. Otherwise the task is
   *unscheduled* (listed but not drawn). Milestones are 0.
4. **CPM forward/backward pass** on working-day calendars (✅ whole working days in v1; hours only for effort) (the task's inherited calendar)
   gives early/late start and finish, total float and the critical path. Inputs are
   FS/SS/FF/SF dependencies with lag, `start` (start-no-earlier-than) and the parent's start.
   Manual tasks are fixed points. Summary tasks span min(child start) to max(child finish).
   Their effort is summed and their progress is effort-weighted.
5. **Resource load.** Per resource per working day, Σ units assigned compared to capacity,
   giving an over-allocation report. No automatic levelling in v1.
6. **Variance.** Deadline misses, plus actual vs. planned dates.

❓ Baselines (named snapshots of planned dates for planned-vs-actual Gantt bars) come later.
They can be stored as a `baseline` op-pack on the root ticket.

## 6. Sync ✅ via `git` subprocess

```
pull:  git fetch <remote> +refs/tix/*:refs/remotes/<remote>/tix/*
       for each entity ref fetched:
         validate every new op-pack (schema, sizes, author rules)
         local missing     → create local ref
         remote ⊆ local    → nothing
         local ⊆ remote    → fast-forward (CAS)
         diverged          → merge commit with both parents (header-only pack)
       advance the Lamport clock
push:  git push <remote> refs/tix/*:refs/tix/*     (never forced)
       rejected → pull, retry
```

- Object and ref access run in-process through libgit.a. Network goes through a child `git`
  process (the same binary version as the pinned library). Credential helpers, ssh config and
  remote helpers then behave exactly as the user expects, and a transport `die()` can't take
  down tix.
- The child is spawned with argv directly, never through a shell. Its stderr is captured into
  `tix_error`.

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
- Verified by `spikes/m0/roundtrip.sh`: CAS refusal, `fsck --strict` clean, two-clone round
  trip through a bare remote, and a **SHA-256 + reftable** repo.

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
```

- **Inputs from outside**: the clock (`now`), randomness (create-op nonces) and the chosen
  identity all come in through `tix_repo_config`. Core never reads env, time or config itself.
- **Local state** goes through the store port (`local_get`/`local_set`). The Lamport clock is
  `$GIT_DIR/tix/clock` in store_libgit, written under git's lockfile.
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

## 9. CLI v1

M2 ✅ implements `id`, `new`, `ls`, `show`, `comment` (incl. `--edit COMMENT-ID`) and `edit`.
The rest arrives with M3 (sync) and M4 (planning). `$EDITOR` resolution: `TIX_EDITOR`, then
`core.editor`, then `VISUAL`, then `EDITOR`, then `vi`. Lines starting with `#` are dropped. Exit codes: 0 ok, 1 error, 2 usage.

```
tix id    new | ls | show <id> | use <id> | edit <id> [--name] [--email] [+key/-key]
tix new   [-t title] [-l label]... [--parent P] [--kind milestone]
tix ls    [--status s] [--label l] [--assignee r] [--under P]
tix show  <id>
tix comment <id> [-m msg]
tix edit  <id> [--title] [--status] [+label|-label] [--parent P] [--rank after:X]
               [--start D] [--finish D] [--duration 3d] [--effort 16h] [--deadline D]
               [--progress 40] [--mode auto|manual] [--dep +P:FS+2d | -P]
tix assign <id> <resource>[:50%] [--effort 8h]     tix unassign <id> <resource>
tix res   new | ls | show | edit   (name, kind, capacity, calendar, identity, rate)
tix cal   new | ls | show | edit   (weekdays, hours, +exception/-exception)
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
4. **M3 sync**: pull/merge/push against Forgejo.
5. **M4 planning**: WBS/scheduling/assignment fields, resource and calendar entities, `plan`,
   `tix wbs/plan/assign/res/cal`, `export`.
6. **M5 index**: SQLite cache and queries.
7. Later: signature enforcement, baselines, TUI, Forgejo issue bridge, MS Project XML export.

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
