#!/usr/bin/env python3
"""
A fake Forgejo API for the adapter test: serves one repository (me/proj) on 127.0.0.1,
runs the test binary (argv[1]) against it, then checks what the binary sent. Stdlib only.
"""
import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

BASE = "/api/v1/repos/me/proj"
seen = []          # (method, path, body)
problems = []


def issue(n, title, body="", state="open", labels=(), pr=False):
    d = {"number": n, "title": title, "body": body, "state": state,
         "labels": [{"id": i + 1, "name": l} for i, l in enumerate(labels)],
         "pull_request": {"merged": False} if pr else None}
    return d


def page_of_issues(page):
    if page == 1:
        items = [issue(1, "First", "body one", labels=["bug"]),
                 issue(2, "Made", "made by tix\n\n<!-- tix:0123456789abcdef -->"),
                 issue(3, "A pull request", pr=True),
                 {"number": 4, "state": "open"}]  # malformed: no title
        items += [issue(n, f"Issue {n}") for n in range(5, 5 + 46)]
        return items  # 50 entries, 48 usable
    if page == 2:
        return [issue(n, f"Issue {n}", state="closed") for n in range(51, 64)]  # 13
    return []


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, obj=None, raw=None, headers=()):
        data = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_any(self, method):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode() if n else ""
        seen.append((method, u.path, body))
        if self.headers.get("Authorization") != "token s3cret":
            problems.append(f"bad auth on {method} {u.path}: {self.headers.get('Authorization')!r}")
            return self.reply(401, {"message": "unauthorized"})
        if not u.path.startswith(BASE):
            return self.reply(404, {"message": "no such repo"})
        p = u.path[len(BASE):]
        page = int(q.get("page", ["1"])[0])
        if method == "GET" and p == "/labels":
            return self.reply(200, [{"id": 1, "name": "bug"}, {"id": 2, "name": "ui"}] if page == 1 else [])
        if method == "GET" and p == "/issues":
            if q.get("state") != ["all"] or q.get("type") != ["issues"]:
                problems.append(f"issues query: {u.query}")
            return self.reply(200, page_of_issues(page))
        if method == "POST" and p == "/issues":
            d = json.loads(body)
            if d.get("title") != 'New "one"' or d.get("labels") != [1] or \
                    d.get("body") != "text\n\n<!-- tix:abcdef0123456 -->":
                problems.append(f"create: {d}")
            return self.reply(201, {"number": 100})
        if method == "PATCH" and p == "/issues/100":
            d = json.loads(body)
            if d != {"body": "edited\n\n<!-- tix:abcdef0123456 -->", "state": "closed"}:
                problems.append(f"update: {d}")
            return self.reply(201, {"number": 100})
        if method == "PUT" and p == "/issues/100/labels":
            if json.loads(body) != {"labels": [1]}:
                problems.append(f"labels: {body}")
            return self.reply(200, [])
        if method == "PATCH" and p == "/issues/404":
            return self.reply(404, {"message": "no such issue\x1b[31m"})
        if method == "GET" and p == "/issues/1/comments":
            return self.reply(200, [{"id": 10, "body": "hi", "user": {"login": "bob"}},
                                    {"id": 11, "body": "x\n\n<!-- tix:abc1234:3 -->", "user": {"login": "tix"}}]
                              if page == 1 else [])
        if method == "POST" and p == "/issues/1/comments":
            if json.loads(body) != {"body": "from tix\n\n<!-- tix:abc1234:4 -->"}:
                problems.append(f"comment: {body}")
            return self.reply(201, {"id": 500})
        if method == "GET" and p == "/issues/666/comments":
            return self.reply(302, {}, headers=[("Location", "http://127.0.0.1:1/steal")])
        if method == "GET" and p == "/issues/777/comments":
            return self.reply(200, raw=b"<html>not json")
        problems.append(f"unexpected {method} {self.path}")
        return self.reply(404, {"message": "unexpected"})

    def do_GET(self):
        self.handle_any("GET")

    def do_POST(self):
        self.handle_any("POST")

    def do_PATCH(self):
        self.handle_any("PATCH")

    def do_PUT(self):
        self.handle_any("PUT")


def main() -> int:
    srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{srv.server_address[1]}/me/proj"
    r = subprocess.run([sys.argv[1], url], capture_output=True, text=True, timeout=60)
    srv.shutdown()
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if not any(m == "GET" and p.endswith("/issues") for m, p, _ in seen):
        problems.append("never listed issues")
    if any("steal" in p for _, p, _ in seen):
        problems.append("followed a redirect")
    for p in problems:
        print("FAIL:", p)
    return 1 if r.returncode or problems else 0


if __name__ == "__main__":
    sys.exit(main())
