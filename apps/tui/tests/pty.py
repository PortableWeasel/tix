#!/usr/bin/env python3
"""
tix-tui in a pseudo-terminal, read back through a terminal emulator (pexpect + pyte).
Every edit is checked with the CLI. argv: TIX TIX-TUI.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

try:
    import pexpect
    import pyte
except ImportError:
    print("pexpect/pyte missing: skipped")
    sys.exit(77)

COLS, ROWS = 120, 30
fails = []


def main() -> int:
    tix, tui = map(os.path.abspath, sys.argv[1:3])
    tmp = tempfile.mkdtemp(prefix="tix-tui-")
    env = dict(os.environ, GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1", TIX_TODAY="2026-11-09",
               # vt220: its terminfo has no indn/rin, so ncurses doesn't scroll with CSI n S / T,
               # which pyte doesn't implement (real terminals do; with xterm the test was flaky)
               TERM="vt220", LANG="C.UTF-8", LC_ALL="C.UTF-8",
               TIX_EDITOR="sed -i '1s/^/Body from the editor/'")

    def run(*a):
        r = subprocess.run([tix, *a], cwd=tmp, env=env, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f"tix {a}: {r.stdout}{r.stderr}")
        return r.stdout

    def new(*a):
        return run("new", *a).split()[-1]

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            fails.append(what)

    for c in (["git", "init", "-q", "."], ["git", "config", "user.name", "Ada"],
              ["git", "config", "user.email", "ada@example.org"]):
        subprocess.run(c, cwd=tmp, env=env, check=True)
    run("id", "new")
    proj = new("-t", "Project", "--start", "2026-11-09")
    new("-t", "Design", "--parent", proj, "--rank", "last", "--duration", "3d")
    new("-t", "Build", "--parent", proj, "--rank", "last", "--duration", "4d")
    alpha = new("-t", "Alpha task", "-l", "bug")
    new("-t", "Beta task")
    new("-t", "Evil ‮override title")

    screen = pyte.Screen(COLS, ROWS)
    stream = pyte.ByteStream(screen)
    child = pexpect.spawn(tui, cwd=tmp, env=env, dimensions=(ROWS, COLS))

    def pump(t=0.1):
        try:
            while True:
                data = child.read_nonblocking(65536, timeout=t)
                if os.environ.get("TUI_RAW_LOG"):
                    with open(os.environ["TUI_RAW_LOG"], "ab") as f:
                        f.write(data)
                stream.feed(data)
                t = 0.05
        except (pexpect.TIMEOUT, pexpect.EOF):
            pass

    def text():
        return "\n".join(screen.display)

    def wait(pred, what, timeout=6.0):
        end = time.time() + timeout
        while time.time() < end:
            pump()
            if pred(text()):
                return True
        check(False, what + "\n" + text())
        return False

    def keys(s, settle=0.15):
        child.send(s)
        time.sleep(settle)
        pump()

    def show(t):
        return run("show", t)

    try:
        wait(lambda s: "Alpha task" in s and "Beta task" in s, "the list shows tickets")
        check("‮" not in text() and "Evil ?override" in text(), "a bidi override is drawn as '?'")
        keys("/beta")
        wait(lambda s: "Beta task" in s and "Alpha task" not in s, "the filter narrows as you type")
        keys("\r")
        keys("\x1b", 0.3)
        wait(lambda s: "Alpha task" in s, "Esc clears the filter")
        keys("/alpha\r")
        keys("\r")
        wait(lambda s: "status:   open" in s and "labels:   bug" in s, "Enter opens the ticket")
        keys("s")
        wait(lambda s: "o open" in s, "s asks for a status")
        keys("d")
        wait(lambda s: "status: done" in s, "the status is saved")
        check("status:   done" in show(alpha), "...and the CLI sees it")
        keys("c")
        keys("hello from the tui\r", 0.4)
        check("hello from the tui" in show(alpha), "c adds a comment")
        keys("e")
        keys("\x7f" * 20 + "Alpha renamed\r", 0.4)
        check(show(alpha).splitlines()[0] == "Alpha renamed", "e edits the title")
        keys("l")
        keys("+ui\r", 0.4)
        check(re.search(r"labels:\s+bug, ui", show(alpha)) is not None, "l adds a label")
        keys("E", 0.8)
        wait(lambda s: "Body from the editor" in s, "E edits the body in $EDITOR")
        check("Body from the editor" in show(alpha), "...saved")
        keys("\x1b", 0.3)
        keys("a")
        wait(lambda s: "Alpha renamed" in s, "a shows done tickets too")
        keys("n")
        keys("Gamma from tui\r", 0.5)
        wait(lambda s: "Gamma from tui" in s and "status:   open" in s, "n creates and opens a ticket")
        check("Gamma from tui" in run("ls"), "...the CLI lists it")
        keys("n")
        keys("Gamma child\r", 0.6)
        out = run("wbs")
        check(re.search(r"Gamma from tui[\s\S]*\n\S+\s+\S+\s+Gamma child", out) is not None, "n in a ticket makes a subtask")
        keys("2", 0.5)
        wait(lambda s: "Design" in s and "Build" in s and "critical" in s, "2 shows the WBS")
        check("1970" not in text(), "an unscheduled summary has no dates")
        keys("3", 0.5)
        wait(lambda s: "Project" in s and ("#" in s or "=" in s), "3 shows the Gantt chart")
        keys("?", 0.3)
        wait(lambda s: "tix tui:" in s, "? shows the keys")
        keys("x", 0.3)
        keys("1", 0.3)
        keys("\x1b", 0.3)  # clear the filter
        new("-t", "Made outside")
        wait(lambda s: "Made outside" in s, "a change from the CLI shows up by itself", timeout=8)
        keys("q", 0.5)
        child.expect(pexpect.EOF, timeout=5)
        child.close()
        check(child.exitstatus == 0, "q quits cleanly")
    finally:
        if child.isalive():
            child.terminate(force=True)
        if not fails:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("kept", tmp)
    print(f"{len(fails)} failure(s)" if fails else "tui OK")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
