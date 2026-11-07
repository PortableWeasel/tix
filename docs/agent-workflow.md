# Agent workflow — the reference skill

How people and AI agents hand tickets back and forth with tix (architecture §15). tix only
provides the mechanisms: stages, claims, typed comments, autonomy and the guard. This
document is the **policy** agents follow. Change it freely; no tix release is needed.
Hermes carries a copy as the `tix-agent-workflow` skill.

Commands assume tix ≥ the §15 release, run inside the repository. Agents never run a
command that opens `$EDITOR`: always pass `-t`, `-m` or `--data`.

## Roles

| Role | Who | Works on |
|---|---|---|
| **Person** | a human identity | writes tickets, answers questions, accepts work, decides on aborts |
| **Refiner** | an agent | `open/refine`: turns a rough ticket into a ready one |
| **Implementer** | an agent | `open/ready`, `in-progress/changes-requested`: does the work |
| **Reviewer** | an agent, **a different identity from the implementer** | `in-progress/review` |

Every agent identity declares itself one: `tix id new --name NAME --kind agent`.
Run reviewers under their own identity. If the same identity implements and reviews, the
reviewer can't skip its own work, and `claim --next` would keep handing it back.

## The stage list (install once per project root)

`*` = waits for a person (`--human`).

| Phase | Stages |
|---|---|
| open | `draft`\* 10, `refine` 20, `needs-input`\* 30, `ready` 40 |
| in-progress | `implementing` 10, `review` 20, `changes-requested` 30, `needs-input`\* 40 |
| blocked | `aborted`\* 10 |
| done | `unaccepted`\* 10, `accepted` 20 |

```bash
R=<root ticket id>
tix stage add $R draft --phase open --human --rank 10
tix stage add $R refine --phase open --rank 20
tix stage add $R needs-input --phase open --human --rank 30
tix stage add $R ready --phase open --rank 40
tix stage add $R implementing --phase in-progress --rank 10
tix stage add $R review --phase in-progress --rank 20
tix stage add $R changes-requested --phase in-progress --rank 30
tix stage add $R needs-input --phase in-progress --human --rank 40
tix stage add $R aborted --phase blocked --human --rank 10
tix stage add $R unaccepted --phase done --human --rank 10
tix stage add $R accepted --phase done --rank 20
tix sync
```

Every ticket below `$R` inherits the list. `tix stage ls ID` shows the list that applies.

**Why it is shaped this way**

- `needs-input` exists in both phases, and the phase says where the ticket goes back to:
  `open/needs-input` returns to `refine`, `in-progress/needs-input` to wherever
  implementation stopped.
- `done/unaccepted` lets dependent tickets start (a dependency only needs the `done`
  phase), so waiting for a person's acceptance never stalls the plan.
- `blocked/aborted` stops only its own branch of the plan. The autonomy guard allows it,
  so an agent can always give up on an impossible ticket.

## Who may move a ticket where

| From | To | Who |
|---|---|---|
| `open/draft` | `open/refine` | person ("please refine") |
| `open/refine` | `open/needs-input`, `open/ready`, `blocked/aborted` | refiner |
| `open/needs-input` | `open/refine` | person, after answering |
| `open/ready`, `in-progress/changes-requested` | `in-progress/implementing` | implementer |
| `in-progress/implementing` | `in-progress/review`, `in-progress/needs-input`, `blocked/aborted` | implementer |
| `in-progress/review` | `done/unaccepted` (or `done/accepted`, see `auto-accept`), `in-progress/changes-requested`, `in-progress/needs-input`, `blocked/aborted` | reviewer |
| `in-progress/needs-input` | back to the in-progress stage it came from | person |
| `done/unaccepted` | `done/accepted`, `in-progress/changes-requested` | person |
| `blocked/aborted` | `open/ready` (retry), `open/refine` (rewrite), `cancelled` | person |
| anything | anything | person |

Agents never set `cancelled` or `done/accepted`. The one exception is a ticket labelled
`auto-accept`, which goes from review straight to `done/accepted`. To propose cancelling a
ticket, ask (supervised) or abort with `next=cancel` (autonomous).

To find where an `in-progress/needs-input` ticket came from, use `tix log ID --json`: it
is the in-progress stage in the last `status in-progress/...` entry before
`status in-progress/needs-input`.

## The agent loop (every role)

```bash
tix claim --next --status open/refine --for 30m --json      # refiner
tix claim --next --status open/ready --status in-progress/changes-requested --json   # implementer
tix claim --next --status in-progress/review --json         # reviewer
```

- Exit 0 prints the ticket you now hold. Exit 3 (and `null` with `--json`) means no work:
  wait with `tix watch --max-events 1` (add `--pull 30s` when others push elsewhere), then
  try again.
- `claim --next` syncs through the remote and settles races. A ticket you already hold
  comes back first, so after a crash just run it again.
- **Renew** before the lease runs out: `tix claim ID --for 30m`. A lapsed claim is
  anyone's.
- **Release** when you hand the ticket on: `tix claim ID --release`. Every hand-over
  below ends with a status change, then a release, then `tix sync`.
- Read the whole ticket with `tix show ID --json` (body, comments with `kind` and `data`,
  claim, stage).

## Notes (typed comments)

Write them with `tix note ID --kind KIND [-m TEXT] [--field K=V]... [--item ID=TEXT[=STATE]]...
[--question ID=TEXT]... [--update CID#ITEM=STATE]... [--reply-to CID]`. `-m` is the
readable summary. Field names and ids are `[a-z0-9][a-z0-9-]`; states are like stages.

| Kind | Written by | Data |
|---|---|---|
| `question` | refiner, implementer | `questions`: one per thing you need answered |
| `answer` | person (the web form writes it) | `fields`: question id → answer; `reply-to` the question |
| `assumption` | any agent, autonomous mode | `fields`: `question`, `chosen`, `alternatives`, `confidence` (`low`/`medium`/`high`) |
| `refined` | refiner | `fields`: `summary`; `items`: sub-tickets created (`id` = short id) |
| `submit` | implementer | `fields`: `branch`, `commit` (the head you want reviewed) |
| `review` | reviewer | `fields`: `verdict` (`approve`/`changes`), `commit`, `round`; `items`: findings, state `open` |
| `fix` | implementer | `fields`: `commit`; `updates`: findings → `addressed` or `wontfix` |
| `escalation` | reviewer | `fields`: `reason` (`review-limit`), `rounds`; `items`: the findings that keep coming back |
| `abort` | any agent | `fields`: `reason` (`impossible`/`review-limit`/`needs-human`/`external-blocker`/`budget-exceeded`), `branch`, `commit`, `next` (`rewrite`/`split`/`unblock`/`cancel`); `-m`: what you tried |
| `workaround` | any agent | `fields`: `for` (the other ticket's id), `change` (e.g. `dep-removed`, `stub-added`); on both tickets |

## Refiner

1. Claim from `open/refine`. Read the ticket and its comments, including answers to
   earlier questions.
2. Improve it in place: a clear body with acceptance criteria (`tix edit ID --body ...`),
   sub-tickets (`tix new -t ... --parent ID --rank last --status open/refine`),
   dependencies (`--dep +ID`), labels, and an effort estimate (`--effort 8h`), so the
   Gantt chart works. People can see every edit in the ticket's history.
3. Need answers? Supervised: `tix note ID --kind question -m "..." --question q1=...`,
   then `tix edit ID --status open/needs-input`. Autonomous: write an `assumption`
   instead and carry on (the guard refuses `needs-input` anyway).
4. Done: `tix note ID --kind refined -m "..."`, then `tix edit ID --status open/ready`.
5. Should it be cancelled or rewritten from scratch? Supervised: say so in a `question`
   and move it to `needs-input`. Autonomous: abort with `next=cancel` or `next=rewrite`.

## Implementer

1. Claim from `open/ready` (or `in-progress/changes-requested`), then
   `tix edit ID --status in-progress/implementing`.
2. Work on a branch named `tix/<short id>`. Never commit to the main branch directly.
   Renew the claim while you work.
3. Coming back from `changes-requested`: address each open finding of the latest
   `review`, then post `tix note ID --kind fix --field commit=<sha> --update
   <review-cid>#f1=addressed ...`.
4. Ready for review: `tix note ID --kind submit --field branch=tix/<id> --field
   commit=<sha>`, then `tix edit ID --status in-progress/review`. A new commit after an
   approval needs a new `submit` and another review.
5. Stuck on a question: supervised, ask and move to `in-progress/needs-input`;
   autonomous, write an `assumption` or abort.

## Reviewer and the review rounds

1. Claim from `in-progress/review`. If the latest `submit` is yours, you implemented it:
   release it and stop. Run reviewers under another identity.
2. Review the `commit` named in the latest `submit`. Check the findings the latest `fix`
   says it addressed first.
3. **Count the round.** In `tix log ID --json`, find the last `status open/ready` entry.
   The round is 1 plus the number of `[review]` comments after it whose `verdict` was
   `changes` (read the verdicts in `tix show ID --json`).
4. **Approve**: `tix note ID --kind review --field verdict=approve --field commit=<sha>
   --field round=N`, then `tix edit ID --status done/unaccepted` (or `done/accepted` with
   the `auto-accept` label).
5. **Changes**, while round ≤ max (default 3; a label `max-rounds:N` overrides it):
   `tix note ID --kind review --field verdict=changes --field commit=<sha> --field round=N
   --item f1="..." --item f2="..."`, then `tix edit ID --status
   in-progress/changes-requested`.
6. **Round > max: escalate.** Supervised: write an `escalation` note listing the findings
   that keep coming back, then `in-progress/needs-input`. The person then rewrites
   (`open/refine`, which starts the count again), grants another round
   (`in-progress/changes-requested`), or cancels. Autonomous: abort with
   `reason=review-limit`.
7. Release your claim.

## Autonomous mode

A person runs `tix autonomy ROOT on --until 08:00` (or `--for 10h`), and every agent
below that root then works without a person:

- You can't put a ticket into a `*` stage (`needs-input`, `draft`, `unaccepted`, …): tix
  refuses with exit 1 and `autonomous mode: ...`. That is your signal to decide yourself.
  Write an `assumption` and carry on, or abort.
- `blocked` and `done` stages are always allowed, because they hold nothing up: finish to
  `done/unaccepted` as usual. It waits for a person's acceptance, but dependents can
  start right away.
- **Aborting** is always allowed: `tix note ID --kind abort -m "what I tried" --field
  reason=... --field next=...`, then `tix edit ID --status blocked/aborted`, then release.
  Push your branch first; never merge it.
- **Work around an aborted predecessor** only when the dependent ticket really doesn't
  need it: remove the dependency (`tix edit ID --dep -PRED`) or add a stub ticket, and
  post a `workaround` note on both tickets.
- `tix autonomy ROOT show --json` tells you the mode. If it says `halted`, stop:
  `claim --next` hands out nothing below a halted root, and only a person can resume.

## What the runner does (not the agents)

The runner is whatever starts agents overnight (Hermes cron, a loop script).

- **Per-ticket budget**: if an agent has held one ticket for more than 2 hours of renewed
  leases, stop it and abort the ticket with `reason=budget-exceeded`.
- **Circuit breaker**: after 3 aborts in a row, or more than half of the last 10 tickets
  aborted, run `tix autonomy ROOT halt` and stop starting agents. Something systemic is
  wrong (a broken build, an expired token, a bad early assumption).
- It never turns autonomy on or off. That is a person's call.

## The morning

`/activity` in `tix web` (or `tix log --since 10h --under ROOT`) shows what happened since
autonomy was switched on. `/inbox` lists what waits for you. Go through it in this order:

1. aborted tickets (their `abort` notes),
2. workarounds,
3. assumptions, lowest confidence first,
4. tickets in `done/unaccepted` to accept or reopen.
