#!/usr/bin/env python3
"""contrib/night.py end to end: real tix, a bare remote, a person and two agent clones, and a fake agent.

    uv run contrib/tests/night_test.py TIX
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
NIGHT = os.path.join(HERE, "..", "night.py")


def main() -> int:
    tix = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="tix-night-")
    env = dict(os.environ, GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1", TIX_NO_INDEX="1", TZ="UTC")
    fails = []

    def run(cwd, *args, ok=(0,)):
        r = subprocess.run(list(args), cwd=cwd, env=env, capture_output=True, text=True)
        if r.returncode not in ok:
            raise RuntimeError(f"{args} in {cwd}: {r.returncode} {r.stdout}{r.stderr}")
        return r.stdout

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            fails.append(what)

    def new(cwd, *args):
        return run(cwd, tix, "new", *args).split()[-1]

    try:
        remote = os.path.join(tmp, "remote.git")
        run(tmp, "git", "init", "-q", "--bare", remote)
        person = os.path.join(tmp, "person")
        run(tmp, "git", "init", "-q", person)
        run(person, "git", "remote", "add", "origin", remote)
        run(person, "git", "config", "user.name", "P")
        run(person, "git", "config", "user.email", "p@x")
        run(person, tix, "id", "new", "--name", "Person")
        root = new(person, "-t", "Night")
        for name, phase, human in [("needs-input", "open", True), ("ready", "open", False),
                                   ("implementing", "in-progress", False), ("review", "in-progress", False),
                                   ("changes-requested", "in-progress", False), ("aborted", "blocked", True),
                                   ("unaccepted", "done", True)]:
            run(person, tix, "stage", "add", root, name, "--phase", phase, *(["--human"] if human else []))
        good = [new(person, "-t", f"Job {i}", "--parent", root, "--rank", "last", "--status", "open/ready")
                for i in range(2)]
        bad = new(person, "-t", "IMPOSSIBLE thing", "--parent", root, "--rank", "last", "--status", "open/ready")
        run(person, tix, "sync")

        clones = {}
        for role, name in [("implement", "Impl"), ("review", "Rev")]:
            c = os.path.join(tmp, role)
            run(tmp, "git", "clone", "-q", remote, c)
            run(c, "git", "config", "user.name", name)
            run(c, "git", "config", "user.email", f"{name}@x")
            run(c, tix, "pull")
            run(c, tix, "id", "new", "--name", name, "--kind", "agent")
            run(c, tix, "sync")
            clones[role] = c

        agent = os.path.join(tmp, "fake-agent")
        with open(agent, "w") as f:
            f.write(textwrap.dedent(f"""\
                #!/bin/sh
                # fake agent: $1 = role, $2 = ticket; an IMPOSSIBLE ticket is left alone
                T={tix}
                title=$("$T" show "$2" | head -1)
                case "$title" in *IMPOSSIBLE*) exit 1 ;; esac
                if [ "$1" = implement ]; then
                  "$T" edit "$2" --status in-progress/implementing
                  "$T" note "$2" --kind submit --field branch=tix/"$2" --field commit=abc1234
                  "$T" edit "$2" --status in-progress/review
                else
                  "$T" note "$2" --kind review --field verdict=approve --field commit=abc1234 --field round=1
                  "$T" edit "$2" --status done/unaccepted
                fi
                "$T" claim "$2" --release
                "$T" sync
                """))
        os.chmod(agent, 0o755)
        cmd = [sys.executable, NIGHT, "--root", root, "--tix", tix, "--poll", "1", "--until-idle", "--quiet",
               "--worker", f"implement:{clones['implement']}", "--worker", f"review:{clones['review']}",
               "--agent", agent + " {role} {ticket}", "--log", os.path.join(tmp, "night.log")]

        # supervised: the runner does nothing
        out = run(tmp, *cmd)
        check("autonomy is supervised: stopping" in out, "the runner won't start without autonomy")

        run(person, tix, "autonomy", root, "on", "--for", "2h")
        run(person, tix, "sync")
        out = run(tmp, *cmd)
        run(person, tix, "pull")
        states = {t: json.loads(run(person, tix, "show", t, "--json")) for t in good + [bad]}
        check(all((states[t]["status"], states[t]["stage"]) == ("done", "unaccepted") for t in good),
              "two jobs implemented and reviewed to done/unaccepted")
        check((states[bad]["status"], states[bad]["stage"]) == ("blocked", "aborted"),
              "the job the agent couldn't do was aborted by the runner")
        notes = [c for c in states[bad]["comments"] if c.get("kind") == "abort"]
        check(bool(notes) and "left the ticket where it found it" in notes[0]["body"], "with an abort note")
        check(all(not states[t]["claim"] for t in good + [bad]), "no claims left behind")
        check("done: claimed 5, handed_on 4, aborted 1" in out, "the summary counts it all: " + out.splitlines()[-1])

        # the circuit breaker: three impossible jobs in a row halt autonomy
        for i in range(3):
            new(person, "-t", f"IMPOSSIBLE {i}", "--parent", root, "--rank", "last", "--status", "open/ready")
        run(person, tix, "sync")
        out = run(tmp, *cmd, ok=(2,))
        check("circuit breaker" in out, "three aborts in a row trip the breaker")
        run(person, tix, "pull")
        mode = json.loads(run(person, tix, "autonomy", root, "show", "--json"))["mode"]
        check(mode == "halted", "and halt autonomy for everyone")
        out = run(clones["implement"], tix, "claim", "--next", "--status", "open/ready", "--json", ok=(3,))
        check(out.strip() == "null", "so claim --next hands out nothing more")

        # the budget: an agent that runs too long is stopped and its ticket aborted
        run(person, tix, "autonomy", root, "on", "--for", "2h")
        slow = new(person, "-t", "Slow job", "--parent", root, "--rank", "first", "--status", "open/ready")
        run(person, tix, "sync")
        sleepy = os.path.join(tmp, "sleepy-agent")
        with open(sleepy, "w") as f:
            f.write("#!/bin/sh\nsleep 600\n")
        os.chmod(sleepy, 0o755)
        bcmd = [sys.executable, NIGHT, "--root", root, "--tix", tix, "--poll", "1", "--until-idle", "--quiet",
                "--worker", f"implement:{clones['implement']}", "--agent", sleepy, "--budget", "0"]
        out = run(tmp, *bcmd, ok=(0, 2))
        run(person, tix, "pull")
        st = json.loads(run(person, tix, "show", slow, "--json"))
        reasons = [c["data"] for c in st["comments"] if c.get("kind") == "abort"]
        check((st["status"], st["stage"]) == ("blocked", "aborted") and any("budget-exceeded" in d for d in reasons),
              "an agent over its budget is stopped and the ticket aborted (budget-exceeded)")
    finally:
        if not fails:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("kept", tmp)
    print("night runner OK" if not fails else f"{len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
