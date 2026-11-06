#!/usr/bin/env bash
# M0 acceptance: in-process object/ref writes via libgit.a, CAS semantics,
# SHA-256 + reftable repos, and a push/fetch round trip through a bare remote.
set -euo pipefail
M0=$(realpath "$1")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=tix GIT_AUTHOR_EMAIL=tix@example.invalid
export GIT_COMMITTER_NAME=tix GIT_COMMITTER_EMAIL=tix@example.invalid
fail() { echo "FAIL: $*" >&2; exit 1; }
R=refs/tix/tickets/t1

git init -q --bare "$T/remote.git"
for c in a b; do git init -q "$T/$c"; git -C "$T/$c" remote add origin "$T/remote.git"; done

cd "$T/a"
c1=$("$M0" write $R)
[ "$(git rev-parse $R)" = "$c1" ] || fail "ref not at new commit"
"$M0" show $R | grep -q '^parents=0$' || fail "root commit has parents"
"$M0" show $R | grep -q '"op":"comment"' || fail "ops.ndjson content"
git cat-file -p "$c1^{tree}" | grep -q 'ops.ndjson' || fail "tree layout"
git fsck --no-dangling --strict --no-progress >/dev/null 2>&1 || fail "fsck"

# CAS: creating an existing ref, or a stale expected value, must be refused
rc=0; "$M0" write $R - 2>/dev/null || rc=$?; [ $rc = 2 ] || fail "create-over-existing allowed (rc=$rc)"
rc=0; "$M0" write $R "$(git hash-object -t blob /dev/null)" 2>/dev/null || rc=$?
[ $rc = 2 ] || fail "stale CAS allowed (rc=$rc)"
[ "$(git rev-parse $R)" = "$c1" ] || fail "ref moved by refused CAS"

# round trip through the bare remote via `git` subprocess
"$M0" push origin
cd "$T/b"
"$M0" fetch origin
[ "$(git rev-parse refs/remotes/origin/tix/tickets/t1)" = "$c1" ] || fail "fetch"
git update-ref $R "$c1"
c2=$("$M0" write $R "$c1")
"$M0" show $R | grep -q '^parents=1$' || fail "child commit parent"
"$M0" push origin
cd "$T/a"
"$M0" fetch origin
[ "$(git rev-parse refs/remotes/origin/tix/tickets/t1)" = "$c2" ] || fail "fetch back"

# repo formats libgit2 lacks or treats as experimental
git init -q --object-format=sha256 --ref-format=reftable "$T/s"
cd "$T/s"
c=$("$M0" write $R)
[ ${#c} = 64 ] || fail "sha256 oid length ${#c}"
"$M0" show $R | grep -q '"tix":1' || fail "sha256/reftable show"
git fsck --no-dangling --strict --no-progress >/dev/null 2>&1 || fail "fsck sha256"

echo "M0 OK: c1=$c1 c2=$c2 sha256=$c"
