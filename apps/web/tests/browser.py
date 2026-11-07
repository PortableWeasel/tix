#!/usr/bin/env python3
"""
Browser test of the M7 enhancements (app.js) against a real tix-web in a throwaway repo.

    uv run --with playwright apps/web/tests/browser.py build/dev/apps/cli/tix build/dev/apps/cli/tix-web

Needs Google Chrome (or Chromium) at CHROME (default /usr/bin/google-chrome). Exits 77 when
no browser is available, so CTest can report it as skipped. Checks that every request the
page makes succeeds, that the CSP is never violated, and that each enhancement changes the
repository the way the equivalent form would (read back with the CLI).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SKIP = 77


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: browser.py TIX TIX-WEB", file=sys.stderr)
        return 2
    tix, web = map(os.path.abspath, sys.argv[1:])
    chrome = os.environ.get("CHROME", "/usr/bin/google-chrome")
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("playwright not installed: skipped")
        return SKIP
    if not os.path.exists(chrome):
        print(f"no browser at {chrome}: skipped")
        return SKIP

    tmp = tempfile.mkdtemp(prefix="tix-browser-")
    env = dict(os.environ, GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1", TIX_TODAY="2026-11-09",
               TZ="UTC")
    fails = []

    def run(*args: str) -> str:
        r = subprocess.run(list(args), cwd=tmp, env=env, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f"{args}: {r.stdout}{r.stderr}")
        return r.stdout

    def check(cond: bool, what: str) -> None:
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            fails.append(what)

    def until(page, js: str, timeout: float = 6.0) -> bool:
        """Polls a JS expression (page.wait_for_function evals, which the CSP forbids)."""
        end = time.time() + timeout
        while time.time() < end:
            if page.evaluate(js):
                return True
            time.sleep(0.1)
        return False

    def new(*args: str) -> str:
        return run(tix, "new", *args).split()[-1]

    def show(t: str) -> str:
        return run(tix, "show", t)

    srv = None
    try:
        run("git", "init", "-q", ".")
        run("git", "config", "user.name", "Ada")
        run("git", "config", "user.email", "ada@example.org")
        run(tix, "id", "new")
        root = new("-t", "House", "--start", "2026-11-09")
        a = new("-t", "Dig foundations", "--parent", root, "--rank", "last", "--duration", "3d")
        b = new("-t", "Pour slab", "--parent", root, "--rank", "last", "--duration", "2d")
        c = new("-t", "Frame walls", "--parent", root, "--rank", "last", "--duration", "4d", "--mode", "manual",
                "--start", "2026-11-16", "--finish", "2026-11-19")
        hostile = new("-t", 'Quote " <img src=x onerror=alert(1)> search-me')

        srv = subprocess.Popen([web, "--listen", "127.0.0.1:0", "--no-open"], cwd=tmp, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        line = srv.stdout.readline()
        m = re.search(r"(http://127\.0\.0\.1:\d+)/login\?t=\S+", line)
        if not m:
            print("tix-web did not start:", line, srv.stderr.read(), file=sys.stderr)
            return 1
        base, login = m.group(1), m.group(0)

        with sync_playwright() as p:
            browser = p.chromium.launch(executable_path=chrome)
            page = browser.new_page(viewport={"width": 1400, "height": 900})
            problems = []
            page.on("console", lambda msg: problems.append(msg.text) if msg.type == "error" else None)
            page.on("pageerror", lambda exc: problems.append(str(exc)))
            page.on("response", lambda r: problems.append(f"{r.status} {r.url}")
                    if r.status >= 400 and r.status != 422 else None)
            page.goto(login)
            check(page.url.endswith("/tickets"), "login lands on the ticket list")
            check(page.evaluate("document.body.classList.contains('js')"), "app.js ran")

            # search as you type: header box, JSON results, Enter opens
            page.fill("form.search input[name=q]", "search-me")
            page.wait_for_selector(".picks li[role=option]")
            li = page.inner_text(".picks li[role=option]")
            check("<img" in li and hostile[:7] in li, "search shows the hostile title as text")
            check(page.locator(".picks img").count() == 0, "no markup injected from a title")
            page.keyboard.press("ArrowDown")
            page.keyboard.press("Enter")
            page.wait_for_url(re.compile(r"/t/" + hostile[:7]))
            check(True, "Enter on a result opens the ticket")

            # inline status on the list
            page.goto(base + "/tickets")
            sel = page.locator(f'tr[data-id^="{hostile}"] select.inline-status')
            sel.select_option("in-progress")
            page.wait_for_selector(f'tr[data-id^="{hostile}"][data-status="in-progress"]')
            check("status:   in-progress" in show(hostile), "inline status change is saved")

            # the ticket page's status form applies on change
            page.goto(f"{base}/t/{b}")
            check(page.locator('form select[name=status] ~ button, form button[hidden]').count() >= 1,
                  "the Set button is hidden")
            page.select_option("form select[name=status]", "blocked")
            until(page, "document.querySelector('select[name=status]').value === 'blocked'")
            time.sleep(0.3)
            check("status:   blocked" in show(b), "ticket page status applies on change")

            # Gantt: drag a manual bar two columns right (start and finish move)
            page.goto(base + "/plan")
            colw = int(page.get_attribute("svg.gantt", "data-colw"))
            bar = page.locator(f'a.task[data-id^="{c}"] rect.bar')
            box = bar.bounding_box()
            scale = box["width"] / float(bar.get_attribute("width"))
            y = box["y"] + box["height"] / 2
            page.mouse.move(box["x"] + 5, y)
            page.mouse.down()
            page.mouse.move(box["x"] + 5 + colw * scale * 1, y, steps=4)
            page.mouse.move(box["x"] + 5 + colw * scale * 2, y, steps=4)
            page.mouse.up()
            until(page, f"(document.querySelector('a.task[data-id^=\"{c}\"]')||{{dataset:{{}}}}).dataset.start === '2026-11-18'")
            s = show(c)
            check("start:    2026-11-18" in s and "finish:   2026-11-21" in s, "drag moves a manual task")

            # resize an auto task by one working day (duration 3 -> 4)
            bar = page.locator(f'a.task[data-id^="{a}"] rect.bar')
            box = bar.bounding_box()
            y = box["y"] + box["height"] / 2
            x_end = box["x"] + box["width"] - 2
            page.mouse.move(x_end, y)
            page.mouse.down()
            page.mouse.move(x_end + colw * scale, y, steps=5)
            page.mouse.up()
            until(page, f"document.querySelector('a.task[data-id^=\"{a}\"]').dataset.dur === '4'")
            check("duration: 4" in show(a) or "4 working days" in show(a) or "4d" in show(a),
                  "dragging the end sets the duration")

            # link: drag Dig's handle onto Frame walls -> Frame waits for Dig (FS)
            page.hover(f'a.task[data-id^="{a}"] rect.bar')
            h = page.locator(f'a.task[data-id^="{a}"] circle.link-h').bounding_box()
            t = page.locator(f'a.task[data-id^="{c}"] rect.bar').bounding_box()
            page.mouse.move(h["x"] + h["width"] / 2, h["y"] + h["height"] / 2)
            page.mouse.down()
            page.mouse.move(t["x"] + t["width"] / 2, t["y"] + t["height"] / 2, steps=8)
            page.mouse.up()
            page.wait_for_selector("#tix-toast:not([hidden])")
            time.sleep(0.5)
            check(a[:7] in show(c), "dragging the handle adds a dependency")

            # keyboard: focus Pour slab (auto), two ArrowRights -> start constraint +2 days
            page.goto(base + "/plan")
            before = page.get_attribute(f'a.task[data-id^="{b}"]', "data-s")
            page.focus(f'a.task[data-id^="{b}"]')
            page.keyboard.press("ArrowRight")
            page.keyboard.press("ArrowRight")
            until(page, f"document.querySelector('a.task[data-id^=\"{b}\"]').dataset.start !== ''")
            want = time.strftime("%Y-%m-%d", time.gmtime(time.mktime(time.strptime(before, "%Y-%m-%d")) + 2 * 86400 + 43200))
            check(f"start:    {want}" in show(b), f"arrow keys move a focused bar (start {want})")
            check(page.evaluate("document.activeElement.dataset.id || ''").startswith(b), "focus stays on the bar")

            # live refresh: a CLI write shows up without reloading
            page.goto(base + "/tickets")
            page.evaluate("window.__marker = 1")
            new("-t", "Arrived live")
            page.wait_for_selector("text=Arrived live", timeout=6000)
            check(page.evaluate("window.__marker === 1"), "live refresh swapped the page in place")

            # a ticket page refreshes for its own ticket (and ones it links), not for others (§15.6)
            page.goto(f"{base}/t/{a}")
            page.evaluate("window.__marker = 2")
            r0 = page.evaluate("+(document.body.dataset.refreshes || 0)")
            s0 = page.evaluate("document.body.dataset.stamp")
            new("-t", "Unrelated elsewhere")
            moved = until(page, f"document.body.dataset.stamp !== '{s0}'")
            check(moved and page.evaluate(f"+(document.body.dataset.refreshes || 0) === {r0}"),
                  "an unrelated change only moves the stamp")
            run(tix, "comment", a, "-m", "Seen on its own page")
            page.wait_for_selector("text=Seen on its own page", timeout=6000)
            check(page.evaluate("window.__marker === 2"), "its own change refreshes in place")
            # the inbox offers notifications when the browser can show them
            page.goto(base + "/inbox")
            check(page.locator("#tix-notify").count() == 1, "the inbox has a notify button")

            # ... but not while typing: a banner instead
            page.goto(f"{base}/t/{a}/edit")
            page.fill("input[name=title]", "Half-typed")
            new("-t", "Arrived while typing")
            page.wait_for_selector("#tix-banner", timeout=6000)
            check(page.input_value("input[name=title]") == "Half-typed", "typing is never thrown away")

            # shortcuts
            page.goto(base + "/tickets")
            page.keyboard.press("?")
            check(page.locator("dialog#tix-help[open]").count() == 1, "? opens the shortcut help")
            page.keyboard.press("Escape")
            until(page, "!document.querySelector('dialog#tix-help[open]')")
            page.keyboard.press("j")
            sel_ok = until(page, "document.querySelectorAll('tr.sel').length === 1", 2.0)
            check(sel_ok, "j selects a row" + ("" if sel_ok else " " + str(page.evaluate(
                "[document.activeElement.tagName, document.querySelectorAll('tr[data-id]').length, "
                "!!document.querySelector('#tix-banner'), !!document.querySelector('dialog[open]'), location.href, "
                "document.body.dataset.refreshes, document.body.dataset.stamp]"))))
            page.keyboard.press("g")
            page.keyboard.press("w")
            page.wait_for_url(re.compile(r"/wbs$"))
            check(True, "g w goes to the WBS")
            page.keyboard.press("/")
            check(page.evaluate("document.activeElement.name") == "q", "/ focuses the search")

            # pickers on forms
            page.goto(f"{base}/t/{b}")
            pred = page.locator("input[name=pred]")
            if pred.count():
                pred.fill("Dig")
                page.wait_for_selector("input[name=pred] ~ .picks li[role=option]")
                page.keyboard.press("ArrowDown")
                page.keyboard.press("Enter")
                check(pred.input_value() == a[:7], "the dependency picker fills a short id")
            else:
                check(False, "dependency form present")

            page.goto(base + "/plan")
            page.hover(f'a.task[data-id^="{c}"] rect.bar')
            page.screenshot(path=os.path.join(tmp, "plan.png"))
            check(not [x for x in problems if "Content Security Policy" in x], "no CSP violations")
            check(not problems, "no console errors or failed requests: " + "; ".join(problems[:5]))
            browser.close()
    finally:
        if srv:
            srv.terminate()
            srv.wait(timeout=10)
        if not fails and not os.environ.get("TIX_BROWSER_KEEP"):
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("kept", tmp)
    print(f"{len(fails)} failure(s)" if fails else "browser OK")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
