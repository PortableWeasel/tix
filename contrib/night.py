#!/usr/bin/env python3
"""tix night runner: keeps agents working through an autonomous night (architecture §15.7).

Not part of tix: tix provides claims, stages and the autonomy switch; the agent workflow skill
(docs/agent-workflow.md) says how agents behave; this runner only starts them and enforces the
two things agents can't police themselves: a time budget per ticket and a circuit breaker.

Each worker is a role plus a clone whose tix.identity is that role's agent identity
(`tix id new --name ... --kind agent` in the clone). Reviewers need their own identity.

    uv run contrib/night.py --root ROOT \\
        --worker refine:/srv/night/refiner --worker implement:/srv/night/impl \\
        --worker review:/srv/night/review \\
        --agent 'hermes -z {prompt_q} --skills tix,tix-agent-workflow --in {clone_q}'

It runs while ROOT's autonomy is `autonomous` (a person turns it on; the runner never does) and
stops when it expires, is turned off or halted, or on SIGINT/SIGTERM. Every run is logged.
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field

ROLES = {
    "refine": ["open/refine"],
    "implement": ["open/ready", "in-progress/changes-requested"],
    "review": ["in-progress/review"],
}

PROMPT = """You are the {role} agent for tix project {root} in {clone}.
You hold the claim on ticket {ticket} ({title}), currently {state}.
Follow the tix-agent-workflow skill for the {role} role on that ticket only: do the work, hand the
ticket on with the right status and note, release your claim, and run `tix sync`.
Autonomy is on: never move it to a stage that waits for a person; record an assumption or abort.
The runner renews your claim every few minutes; you have {budget_min} minutes in total."""

stopping = False


class Halted(Exception):
    """The circuit breaker tripped."""


def on_signal(_sig, _frm):
    global stopping
    stopping = True


@dataclass
class Worker:
    role: str
    clone: str
    idle_until: float = 0.0


@dataclass
class Breaker:
    """3 aborts in a row, or more than half of the last 10 tickets aborted, trips it."""
    in_a_row: int = 3
    window: int = 10
    recent: list = field(default_factory=list)

    def record(self, aborted: bool) -> bool:
        self.recent = (self.recent + [aborted])[-self.window:]
        streak = 0
        for a in reversed(self.recent):
            if not a:
                break
            streak += 1
        return streak >= self.in_a_row or (
            len(self.recent) == self.window and sum(self.recent) * 2 > self.window)


class Runner:
    def __init__(self, a):
        self.a = a
        self.workers = [Worker(*w.split(":", 1)) for w in a.worker]
        for w in self.workers:
            if w.role not in ROLES:
                raise SystemExit(f"night: unknown role '{w.role}' (refine, implement, review)")
        self.breaker = Breaker()
        self.log_f = open(a.log, "a", encoding="utf-8") if a.log else None
        self.stats = {"claimed": 0, "handed_on": 0, "aborted": 0}

    def log(self, msg: str) -> None:
        line = time.strftime("%Y-%m-%d %H:%M:%S ") + msg
        print(line, flush=True)
        if self.log_f:
            self.log_f.write(line + "\n")
            self.log_f.flush()

    def tix(self, clone: str, *args: str, check: bool = True) -> subprocess.CompletedProcess:
        r = subprocess.run([self.a.tix, *args], cwd=clone, capture_output=True, text=True)
        if check and r.returncode not in (0, 3):
            raise RuntimeError(f"tix {' '.join(args)} in {clone}: {r.stderr.strip()}")
        return r

    def autonomy(self, clone: str) -> str:
        if not self.a.local:
            self.tix(clone, "pull", check=False)
        r = self.tix(clone, "autonomy", self.a.root, "show", "--json")
        return json.loads(r.stdout)["mode"]

    def show(self, clone: str, ticket: str) -> dict:
        return json.loads(self.tix(clone, "show", ticket, "--json").stdout)

    def claim_next(self, w: Worker) -> dict | None:
        args = ["claim", "--next", "--under", self.a.root, "--for", f"{self.a.lease}m", "--json"]
        for st in ROLES[w.role]:
            args += ["--status", st]
        if self.a.local:
            args.append("--local")
        r = self.tix(w.clone, *args)
        return None if r.returncode == 3 else json.loads(r.stdout)

    def abort(self, w: Worker, ticket: str, reason: str, why: str) -> None:
        """The runner's own abort, for a ticket an agent left behind."""
        self.tix(w.clone, "note", ticket, "--kind", "abort", "-m", why, "--field", f"reason={reason}",
                 "--field", "next=rewrite", check=False)
        self.tix(w.clone, "edit", ticket, "--status", "blocked/aborted", check=False)
        self.tix(w.clone, "claim", ticket, "--release", check=False)
        if not self.a.local:
            self.tix(w.clone, "sync", check=False)

    def run_agent(self, w: Worker, t: dict) -> None:
        ticket, before = t["id"], (t["status"], t["stage"])
        state = t["status"] + (f"/{t['stage']}" if t["stage"] else "")
        prompt = PROMPT.format(role=w.role, root=self.a.root, clone=w.clone, ticket=t["short"],
                               title=t["title"], state=state, budget_min=self.a.budget)
        cmd = self.a.agent.format(prompt_q=shlex.quote(prompt), clone_q=shlex.quote(w.clone),
                                  ticket=t["short"], role=w.role)
        self.log(f"{w.role}: {t['short']} {t['title']!r} ({state}) -> agent")
        start = time.time()
        proc = subprocess.Popen(cmd, shell=True, cwd=w.clone, start_new_session=True,
                                stdout=subprocess.DEVNULL if self.a.quiet else None)
        timed_out = False
        renew_every = max(60, self.a.lease * 60 // 3)
        last_renew = time.time()
        while proc.poll() is None:
            time.sleep(min(5, self.a.poll))
            if stopping:
                break
            if time.time() - start > self.a.budget * 60:
                timed_out = True
                break
            if time.time() - last_renew > renew_every:
                self.tix(w.clone, "claim", ticket, "--for", f"{self.a.lease}m", check=False)
                last_renew = time.time()
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
        if not self.a.local:
            self.tix(w.clone, "pull", check=False)
        now = self.show(w.clone, ticket)
        after = (now["status"], now["stage"])
        mine = (now.get("claim") or "") == self.my_identity(w)
        aborted = after == ("blocked", "aborted")
        if timed_out:
            self.abort(w, ticket, "budget-exceeded", f"The agent ran past its {self.a.budget}-minute budget.")
            aborted = True
        elif after == before and not stopping:
            self.abort(w, ticket, "impossible",
                       f"The agent (exit {proc.returncode}) left the ticket where it found it.")
            aborted = True
        elif mine and not aborted:
            self.tix(w.clone, "claim", ticket, "--release", check=False)  # handed on but kept the claim
        self.stats["aborted" if aborted else "handed_on"] += 1
        self.log(f"{w.role}: {t['short']} -> {after[0]}{'/' + after[1] if after[1] else ''}"
                 f"{' (aborted)' if aborted else ''} after {int(time.time() - start)}s")
        if self.breaker.record(aborted):
            self.log("circuit breaker: too many aborts; halting autonomy")
            self.tix(w.clone, "autonomy", self.a.root, "halt", check=False)
            if not self.a.local:
                self.tix(w.clone, "sync", check=False)
            raise Halted

    def my_identity(self, w: Worker) -> str:
        r = subprocess.run(["git", "config", "tix.identity"], cwd=w.clone, capture_output=True, text=True)
        return r.stdout.strip()

    def loop(self) -> int:
        deadline = time.time() + self.a.max_hours * 3600
        while not stopping and time.time() < deadline:
            mode = self.autonomy(self.workers[0].clone)
            if mode != "autonomous":
                self.log(f"autonomy is {mode}: stopping")
                break
            worked = False
            for w in self.workers:
                if stopping or w.idle_until > time.time():
                    continue
                t = self.claim_next(w)
                if not t:
                    w.idle_until = time.time() + self.a.poll
                    continue
                self.stats["claimed"] += 1
                worked = True
                try:
                    self.run_agent(w, t)
                except Halted:
                    return self.finish(2)
            if not worked:
                if self.a.until_idle:
                    self.log("no work for any worker: stopping (--until-idle)")
                    break
                time.sleep(self.a.poll)
        return self.finish(0)

    def finish(self, rc: int) -> int:
        self.log("done: " + ", ".join(f"{k} {v}" for k, v in self.stats.items()))
        return rc


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--root", required=True, help="the WBS root whose autonomy governs the night")
    p.add_argument("--worker", action="append", required=True, metavar="ROLE:CLONE",
                   help="refine|implement|review and a clone set to that agent's identity (repeatable)")
    p.add_argument("--agent", required=True,
                   help="command template; {prompt_q} {clone_q} are shell-quoted, {ticket} {role} plain")
    p.add_argument("--tix", default="tix")
    p.add_argument("--budget", type=int, default=120, help="minutes per ticket (default 120)")
    p.add_argument("--lease", type=int, default=30, help="claim lease in minutes (default 30)")
    p.add_argument("--poll", type=int, default=60, help="seconds between looks when idle (default 60)")
    p.add_argument("--max-hours", type=float, default=14, help="stop after this long regardless")
    p.add_argument("--until-idle", action="store_true", help="stop as soon as no worker finds work")
    p.add_argument("--local", action="store_true", help="claims without syncing (clones that share no remote)")
    p.add_argument("--log", help="append a log here")
    p.add_argument("--quiet", action="store_true", help="discard agent output")
    a = p.parse_args()
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    return Runner(a).loop()


if __name__ == "__main__":
    sys.exit(main())
