#!/usr/bin/env bash
# End-to-end: the tix binary against real git repositories.
set -uo pipefail
TIX=$(realpath "$1")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 TZ=UTC
export GIT_AUTHOR_NAME=git GIT_AUTHOR_EMAIL=git@example.invalid
export GIT_COMMITTER_NAME=git GIT_COMMITTER_EMAIL=git@example.invalid
unset TIX_EDITOR VISUAL EDITOR
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }
# expect <exit-code> <command...>: runs, captures stdout+stderr in $out
expect() { local want=$1; shift; out=$("$@" 2>&1); local rc=$?; [ $rc = "$want" ] || fail "'$*' exited $rc (want $want): $out"; }
has() { grep -qF -- "$1" <<<"$out" || fail "expected '$1' in: $out"; }
hasnt() { ! grep -qF -- "$1" <<<"$out" || fail "did not expect '$1' in: $out"; }

mkdir "$T/nogit" && cd "$T/nogit"
expect 1 "$TIX" ls; has "not a git repository"

git init -q --bare "$T/remote.git"
git init -q "$T/a" && cd "$T/a"
git remote add origin "$T/remote.git"
git config user.name "Ada Lovelace" && git config user.email ada@example.org

expect 2 "$TIX"; has "usage: tix"
expect 0 "$TIX" help; has "statuses:"
expect 2 "$TIX" frobnicate; has "unknown command"
expect 1 "$TIX" new -t "No identity yet"; has "tix id new"

expect 0 "$TIX" id new; has "Created identity"; has "Now writing as"
ada=$(git config tix.identity); [ ${#ada} = 40 ] || fail "tix.identity not set ($ada)"

expect 0 "$TIX" new -t "Crash on save" -m $'Steps:\n1. open\n2. save' -l bug -l ui
t1=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" new -t "Second ticket"
t2=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" ls; has "$t1  open         Crash on save  [bug, ui]"; has "$t2"

expect 0 "$TIX" edit "$t1" --status in-progress +p1 -ui --title "Crash when saving"
expect 0 "$TIX" show "$t1"; has "Crash when saving"; has "status:   in-progress"; has "labels:   bug, p1"
has "author:   Ada Lovelace"; has "    2. save"
expect 0 "$TIX" edit "$t2" --status done
expect 0 "$TIX" ls; hasnt "$t2"
expect 0 "$TIX" ls --all; has "$t2"
expect 0 "$TIX" ls --status done; has "$t2"; hasnt "$t1"
expect 0 "$TIX" ls --label p1 --all; has "$t1"; hasnt "$t2"
expect 0 "$TIX" ls --author "${ada:0:7}" --all; has "$t1"; has "$t2"

expect 0 "$TIX" comment "$t1" -m "looking into it"
expect 0 "$TIX" show "$t1"; has "looking into it"
cid=$(grep -o '^--- [0-9a-f]*:[0-9]*' <<<"$out" | cut -c5-)
expect 0 "$TIX" comment "$t1" --edit "$cid" -m "fixed in a1b2c3"
expect 0 "$TIX" show "$t1"; has "fixed in a1b2c3"; has "(edited)"; hasnt "looking into it"

# usage and id errors
expect 2 "$TIX" edit "$t1"; has "nothing to change"
expect 2 "$TIX" ls --status wip; has "unknown status"
expect 1 "$TIX" show "${t1:0:6}"; has "must be 7"
expect 1 "$TIX" show 0000000; has "no ticket matches"
expect 1 "$TIX" edit "$t1" --title $'two\nlines'; has "control characters"

# editor flow
cat > "$T/editor.sh" <<'ED'
#!/bin/sh
printf 'Title from editor\n\nBody line one\nBody line two\n# dropped comment\n' > "$1"
ED
cat > "$T/empty-editor.sh" <<'ED'
#!/bin/sh
: > "$1"
ED
chmod +x "$T/editor.sh" "$T/empty-editor.sh"
TIX_EDITOR="$T/editor.sh" expect 0 "$TIX" new -l from-editor
t3=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" show "$t3"; has "Title from editor"; has "    Body line two"; hasnt "dropped comment"
before=$(git for-each-ref refs/tix/tickets | wc -l)
TIX_EDITOR="$T/empty-editor.sh" expect 1 "$TIX" new; has "empty title"
[ "$(git for-each-ref refs/tix/tickets | wc -l)" = "$before" ] || fail "aborted editor still created a ticket"

# second identity: switch, edit own identity, may not edit Ada's comment
expect 0 "$TIX" id new --name Bob --email bob@example.org; has "Still writing as ${ada:0:7}"
bob=$(sed -n 's/^Created identity \([0-9a-f]*\).*/\1/p' <<<"$out")
expect 0 "$TIX" id use "$bob"
expect 0 "$TIX" id edit --name "Robert" --add-key ssh SHA256:abc "ssh-ed25519 AAAA bob"
expect 0 "$TIX" id show; has "name:   Robert"; has "(you)"; has "key:    ssh SHA256:abc"
expect 0 "$TIX" id ls; has "* $bob  Robert"; has "  ${ada:0:7}  Ada Lovelace"
expect 1 "$TIX" comment "$t1" --edit "$cid" -m "hijack"; has "only edit your own comments"
expect 0 "$TIX" comment "$t1" -m "Bob agrees"
expect 0 "$TIX" show "$t1"; has "Robert"; has "Bob agrees"
expect 0 "$TIX" id use "${ada:0:7}"

# a hostile/corrupt ref is skipped, not fatal
echo x > f && git add f && git commit -qm normal
git update-ref "refs/tix/tickets/$(git rev-parse HEAD)" HEAD
expect 0 "$TIX" ls --all; has "skipped 1 invalid ticket"; has "$t1"
expect 1 "$TIX" show "$(git rev-parse --short=10 HEAD)"; has "not an op-pack commit"
git update-ref -d "refs/tix/tickets/$(git rev-parse HEAD)"

git fsck --strict --no-dangling --no-progress >/dev/null 2>&1 || fail "git fsck"
[ "$(git ls-tree --name-only "refs/tix/tickets/$(git for-each-ref --format='%(refname:lstrip=3)' refs/tix/tickets | head -1)")" = ops.ndjson ] || fail "tree layout"

# git-tix alias
expect 0 env PATH="$(dirname "$TIX"):$PATH" git tix ls; has "$t1"

# data travels with plain git (sync proper is M3)
git push -q origin 'refs/tix/*:refs/tix/*' || fail "push"
git clone -q "$T/remote.git" "$T/b" 2>/dev/null && cd "$T/b"
git fetch -q origin 'refs/tix/*:refs/tix/*' || fail "fetch"
expect 0 "$TIX" ls --all; has "Crash when saving"; has "Second ticket"
expect 0 "$TIX" show "$t1"; has "author:   Ada Lovelace"; has "Robert"
expect 1 "$TIX" comment "$t1" -m "no identity here"; has "tix id new"
expect 0 "$TIX" id use "${ada:0:7}"
expect 0 "$TIX" comment "$t1" -m "from clone b"

# SHA-256 + reftable repository
git init -q --object-format=sha256 --ref-format=reftable "$T/s" && cd "$T/s"
expect 0 "$TIX" id new --name S --email s@example.org
expect 0 "$TIX" new -t "sha256 ticket"
t=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" show "$t"; has "sha256 ticket"
[ "$(sed -n 's/^id: *//p' <<<"$out" | tr -d '\n' | wc -c)" = 64 ] || fail "sha256 id length"
git fsck --strict --no-dangling --no-progress >/dev/null 2>&1 || fail "git fsck sha256"

if [ $fails -gt 0 ]; then echo "$fails failure(s)"; exit 1; fi
echo "cli end-to-end OK"
