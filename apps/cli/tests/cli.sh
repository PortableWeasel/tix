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
expect 0 "$TIX" --version; has "tix 0."; has "(git.git 2.47.3)"
expect 0 "$TIX" web --help; has "--read-only"   # execs tix-web next to tix
expect 2 "$TIX" web --listen 0.0.0.0:7480; has "hosted mode"
expect 2 "$TIX" tui </dev/null; has "needs a terminal"   # execs tix-tui next to tix
expect 2 "$TIX" web --trust-proxy X-User --listen 0.0.0.0:0; has "needs --public-url https://"
expect 2 "$TIX" web --users /dev/null; has "need --trust-proxy"
expect 2 "$TIX" web --trust-proxy "X User" --public-url https://t.example; has "takes a header name"
expect 2 "$TIX" web --listen 192.168.1.5:80; has "hosted mode"
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
expect 2 "$TIX" ls --sort size; has "--sort is newest"
expect 2 "$TIX" new -t x --status wip; has "unknown status"

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

# ---- sync: push, clone, pull, concurrent edits, merge -----------------------
expect 2 "$TIX" pull --force; has "unknown option"
expect 2 "$TIX" pull origin extra; has "usage: tix pull"
expect 1 "$TIX" pull nosuch; has "no remote named 'nosuch'"
expect 1 "$TIX" pull 'https://example.invalid/x'; has "bad remote name"
git remote add dead "$T/does-not-exist"
expect 1 "$TIX" pull dead; has "git fetch failed"
expect 0 "$TIX" push; has "origin: pushed 5"   # 2 identities + 3 tickets
expect 0 "$TIX" push; has "origin: pushed 0"
expect 0 "$TIX" pull; has "origin: nothing new"
[ ! -e .git/FETCH_HEAD ] || fail "tix fetch wrote FETCH_HEAD"
[ -z "$(git for-each-ref refs/remotes/ --format='%(refname)')" ] || fail "tix fetch wrote refs/remotes/"
[ "$(git for-each-ref refs/tix-remotes/origin/ | wc -l)" = 5 ] || fail "mirror not in refs/tix-remotes/origin/"

git clone -q "$T/remote.git" "$T/b" 2>/dev/null && cd "$T/b"
expect 0 "$TIX" ls --all; hasnt "Crash"   # a plain clone has no tix refs
expect 0 "$TIX" pull; has "origin: 5 new, 0 updated, 0 merged"
expect 0 "$TIX" ls --all; has "Crash when saving"; has "Second ticket"
expect 0 "$TIX" show "$t1"; has "author:   Ada Lovelace"; has "Robert"
expect 1 "$TIX" comment "$t1" -m "no identity here"; has "tix id new"
expect 0 "$TIX" id new --name Carol --email carol@example.org
expect 0 "$TIX" edit "$t1" --status blocked +from-b
(cd "$T/a" && expect 0 "$TIX" edit "$t1" --title "Crash when saving twice" -p1 && expect 0 "$TIX" sync && has "pushed 1") ||
  fail "edit+sync in a"
expect 1 "$TIX" push; has "has changes you don't have"
expect 0 "$TIX" sync; has "0 new, 0 updated, 1 merged"; has "pushed 1"   # the merge; the refused push already sent Carol (not atomic)
cd "$T/a"
expect 0 "$TIX" pull; has "1 new, 1 updated, 0 merged"
expect 0 "$TIX" show "$t1"; a_show=$out
has "Crash when saving twice"; has "status:   blocked"; has "labels:   bug, from-b"
cd "$T/b"
expect 0 "$TIX" show "$t1"
[ "$out" = "$a_show" ] || fail "clones disagree after sync: $(diff <(echo "$a_show") <(echo "$out"))"
[ "$(git rev-parse "refs/tix/tickets/$(git -C "$T/a" for-each-ref --format='%(refname:lstrip=3)' "refs/tix/tickets/$t1*")")" = \
  "$(git -C "$T/a" rev-parse "refs/tix/tickets/$(git -C "$T/a" for-each-ref --format='%(refname:lstrip=3)' "refs/tix/tickets/$t1*")")" ] ||
  fail "clones have different heads"
expect 0 "$TIX" sync; has "nothing new"; has "pushed 0"

# a hostile remote ref is rejected and never adopted; sync still completes
git -C "$T/a" update-ref "refs/tix/tickets/$(git -C "$T/a" rev-parse HEAD)" HEAD
git -C "$T/a" push -q origin "refs/tix/tickets/$(git -C "$T/a" rev-parse HEAD)"
git -C "$T/a" update-ref -d "refs/tix/tickets/$(git -C "$T/a" rev-parse HEAD)"
git --git-dir="$T/remote.git" update-ref refs/tix/widgets/x "$(git -C "$T/a" rev-parse HEAD)"
expect 1 "$TIX" pull; has "warning: rejected refs/tix-remotes/origin/tickets/"; has "not an op-pack commit"
has "1 ref(s) of an unknown kind ignored"
expect 0 "$TIX" ls --all; hasnt "skipped"; [ -z "$(git for-each-ref refs/tix/widgets)" ] || fail "adopted unknown ref"
expect 1 "$TIX" sync; has "pushed 0"
git --git-dir="$T/remote.git" fsck --strict --no-dangling --no-progress >/dev/null 2>&1 || fail "remote fsck"
git fsck --strict --no-dangling --no-progress >/dev/null 2>&1 || fail "clone b fsck"

# a branch literally named "tix" fetches next to the mirror (clashed before 0.6), and a
# pre-0.6 mirror is cleaned up, but only refs shaped like tix mirrors
git --git-dir="$T/remote.git" update-ref refs/heads/tix "$(git -C "$T/a" rev-parse HEAD)"
some=$(git for-each-ref --count=1 --format='%(refname:lstrip=2)' refs/tix/tickets/)
git update-ref "refs/remotes/origin/tix/$some" "refs/tix/$some"
git update-ref refs/remotes/origin/tix/feature HEAD 2>/dev/null || git update-ref refs/remotes/origin/tix/feature "refs/tix/$some"
expect 1 "$TIX" pull; has "rejected"   # the hostile ref is still on the remote
[ -z "$(git for-each-ref "refs/remotes/origin/tix/tickets/")" ] || fail "legacy mirror ref kept"
[ -n "$(git for-each-ref refs/remotes/origin/tix/feature)" ] || fail "real tracking branch deleted"
git update-ref -d refs/remotes/origin/tix/feature
git fetch -q origin 2>&1 || fail "git fetch with a branch named tix"
[ -n "$(git for-each-ref refs/remotes/origin/tix)" ] || fail "branch tix not fetched"
expect 1 "$TIX" pull; has "rejected"; hasnt "git fetch failed"
git --git-dir="$T/remote.git" update-ref -d refs/heads/tix

# tix.remote picks the default remote
git init -q --bare "$T/mirror.git" && git remote add mirror "$T/mirror.git"
git config tix.remote mirror
expect 0 "$TIX" push; has "mirror: pushed 6"
[ "$(git --git-dir="$T/mirror.git" for-each-ref refs/tix | wc -l)" = 6 ] || fail "mirror ref count"
git config --unset tix.remote

# ---- planning: resources, calendars, WBS, CPM, export --------------------------
git init -q "$T/plan" && cd "$T/plan"
export TIX_TODAY=2026-11-09   # a Monday
mk() { sed -n 's/^Created [a-z]* //p' <<<"$out"; }
expect 0 "$TIX" id new --name Ada --email ada@example.org
expect 0 "$TIX" cal new "Site crew" --weekdays mon-sat --off 2026-11-11=Holiday; cal=$(mk)
expect 2 "$TIX" cal new Bad --weekdays mon-xyz; has "--weekdays"
expect 0 "$TIX" cal show "$cal"; has "weekdays: mon tue wed thu fri sat"; has "off:      2026-11-11  Holiday"
expect 0 "$TIX" res new Ada --identity me --rate "95.50 eur"; ada=$(mk)
expect 0 "$TIX" res new Crew --kind role --capacity 300% --calendar "$cal"; crew=$(mk)
expect 2 "$TIX" res new X --capacity 0%; has "--capacity"
expect 0 "$TIX" res show "$ada"; has "rate:     95.50 EUR/h"; has "identity: Ada"
expect 0 "$TIX" res ls; has "$crew  role        300%  Crew"
expect 0 "$TIX" new -t House --calendar "$cal" --deadline 2026-11-20; house=$(mk)
expect 0 "$TIX" new -t "Dig foundations" --parent "$house" --rank last --effort 48h; dig=$(mk)
expect 0 "$TIX" assign "$dig" "$crew"
expect 0 "$TIX" new -t "Pour slab" --parent "$house" --rank "after:$dig" --duration 2d --dep "+$dig"; pour=$(mk)
expect 0 "$TIX" assign "$pour" "$ada:50%" --effort 8h
expect 0 "$TIX" new -t Walls --parent "$house" --rank last --duration 4d --dep "+$pour:FS+1d"; walls=$(mk)
expect 0 "$TIX" new -t Handover --parent "$house" --rank last --kind milestone --dep "+$walls"; hand=$(mk)
expect 0 "$TIX" new -t Paperwork --parent "$house" --rank first --duration 2d; paper=$(mk)
expect 0 "$TIX" assign "$walls" "$ada"
expect 0 "$TIX" show "$pour"; has "parent:   $house House"; has "after:    $dig Dig foundations (FS+0d)"
has "assigned: $ada Ada 50%, 8h"; has "duration: 2d"

# local edits that would break the plan are refused up front
expect 1 "$TIX" edit "$house" --parent "$walls"; has "would make a cycle"
expect 1 "$TIX" edit "$dig" --dep "+$walls"; has "would make a cycle"
expect 1 "$TIX" edit "$dig" --dep "-$walls"; has "no dependency on"
expect 1 "$TIX" unassign "$dig" "$ada"; has "is not assigned"
expect 1 "$TIX" edit "$dig" --rank "after:$cal"; has "no ticket matches"
expect 2 "$TIX" edit "$dig" --duration 3x; has "should look like 3d"
expect 2 "$TIX" edit "$dig" --dep "+$walls:XX"; has "kind must be"
expect 2 "$TIX" edit "$dig" --start 2026-02-30; has "not a date"
expect 2 "$TIX" assign "$dig" "$ada:abc"; has "units must look like"

expect 0 "$TIX" wbs
has "1          $house  House"; has "1.1        $paper    Paperwork"; has "1.2        $dig    Dig foundations"
has "1.3        $pour    Pour slab"; has "1.5        $hand    Handover"
expect 0 "$TIX" plan "$house"
has "1.2        $dig    Dig foundations               2026-11-09  2026-11-10      2      0  critical"
has "1.3        $pour    Pour slab                     2026-11-12  2026-11-13      2      0  critical"  # skips the holiday
has "1.4        $walls    Walls                         2026-11-16  2026-11-19      4      0  critical"  # Sat lag day
has "1.5        $hand    Handover                      2026-11-19  2026-11-19      0      0  critical  milestone"
has "1.1        $paper    Paperwork                     2026-11-09  2026-11-10      2      7"
has "plan: 2026-11-09..2026-11-19"; hasnt "LATE"
expect 0 "$TIX" plan "$house" --critical; hasnt "Paperwork"; has "Walls"
expect 0 "$TIX" plan "$house" --gantt; has "|2026-11-09"; has "Walls                       |       ####"
[ "$(awk 'NF == 0 { exit } /\|/ { print index($0, "|") }' <<<"$out" | sort -u | wc -l)" = 1 ] ||
	fail "gantt header and rows are misaligned: $out"
expect 0 "$TIX" plan "$house" --load; has "Ada"; has "Crew"; hasnt "OVER-ALLOCATED"
expect 0 "$TIX" edit "$walls" --duration 6d
expect 0 "$TIX" plan "$house"; has "LATE"; has "after its deadline 2026-11-20"
expect 0 "$TIX" assign "$paper" "$ada"
expect 0 "$TIX" plan "$house" --load; has "OVER-ALLOCATED"; has "over-allocated"
expect 0 "$TIX" ls --under "$house" --assignee "$ada"; has "Walls"; has "Pour slab"; hasnt "Dig foundations"
expect 0 "$TIX" ls --under "$pour"; [ -z "$out" ] || fail "nothing is under Pour slab: $out"

expect 0 "$TIX" export "$house" --format mermaid
has "gantt"; has "title House"; has "1.3 Pour slab :crit, t$pour, 2026-11-12, 2026-11-14"
has "1.5 Handover :crit, milestone, t$hand"
expect 0 "$TIX" export "$house" --format csv; has "1.3,$pour"; has "Ada:50%"
expect 0 "$TIX" export --format json; has '"tasks":[{"wbs":"1"'; has '"diagnostics":['
expect 0 "$TIX" export "$house" --format mspdi; has '<Project xmlns="http://schemas.microsoft.com/project">'
has "<OutlineNumber>1.3</OutlineNumber>"; has "<PredecessorLink>"; has "<Resources><Resource>"
expect 2 "$TIX" export "$house"; has "--format mermaid|csv|json|mspdi is required"
expect 2 "$TIX" plan --frobnicate; has "unexpected"

# a concurrent re-parent that forms a cycle on merge is broken the same way everywhere
git init -q --bare "$T/plan.git" && git remote add origin "$T/plan.git"
expect 0 "$TIX" sync
git clone -q "$T/plan.git" "$T/plan2" 2>/dev/null
(cd "$T/plan2" && "$TIX" pull >/dev/null && "$TIX" id use "$(git -C "$T/plan" config tix.identity)" >/dev/null &&
  "$TIX" edit "$walls" --parent "$pour" && "$TIX" sync >/dev/null) || fail "clone 2 edit"
expect 0 "$TIX" edit "$pour" --parent "$walls"   # concurrently, the other way round
expect 0 "$TIX" sync; has "1 updated"   # different tickets: no merge, yet a cycle
expect 0 "$TIX" wbs; has "WBS cycle: dropped the parent link"
a_wbs=$out
(cd "$T/plan2" && "$TIX" pull >/dev/null && "$TIX" wbs >"$T/wbs2" 2>&1) || fail "clone 2 wbs"
[ "$a_wbs" = "$(cat "$T/wbs2")" ] || fail "clones plan differently: $(diff <(echo "$a_wbs") "$T/wbs2")"
expect 0 "$TIX" res edit "$crew" --capacity 200% --name "Small crew"
expect 0 "$TIX" cal edit "$cal" --clear 2026-11-11 --on 2026-11-15
expect 0 "$TIX" cal show "$cal"; has "on:       2026-11-15"; hasnt "Holiday"
expect 1 "$TIX" cal edit "$cal" --clear 2026-12-01; has "no such exception"
git fsck --strict --no-dangling --no-progress >/dev/null 2>&1 || fail "plan repo fsck"
unset TIX_TODAY

# ---- identity kind (§15.2) ----------------------------------------------------------
cd "$T/a"
expect 0 "$TIX" id show; has "kind:   human"
expect 2 "$TIX" id edit --kind robot; has "--kind is human or agent"
expect 0 "$TIX" id edit --kind agent
expect 0 "$TIX" id show; has "kind:   agent"
expect 0 "$TIX" id ls; has "(agent)"
expect 0 "$TIX" id edit --kind human
expect 0 "$TIX" id ls; hasnt "(agent)"
expect 0 "$TIX" id new --name "Bot" --email bot@example.invalid --kind agent; has "Created identity"
bot=$(sed -n 's/^Created identity \([0-9a-f]*\).*/\1/p' <<<"$out")
expect 0 "$TIX" id show "$bot"; has "kind:   agent"
expect 2 "$TIX" id new --kind robot; has "--kind is human or agent"

# ---- workflow stages (§15.1): phase/stage, stage lists inherited down the WBS ---------
cd "$T/a"
expect 0 "$TIX" new -t "Workflow root"; wr=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" stage ls "$wr"; has "no stage list applies"
expect 0 "$TIX" stage add "$wr" draft --phase open --human
expect 0 "$TIX" stage add "$wr" ready --phase open
expect 0 "$TIX" stage add "$wr" review --phase in-progress --rank 30
expect 2 "$TIX" stage add "$wr" Bad --phase open; has "stage names are"
expect 2 "$TIX" stage add "$wr" x; has "usage: tix stage add"
expect 2 "$TIX" stage add "$wr" x --phase later; has "unknown phase"
expect 0 "$TIX" stage ls "$wr"; has "open/draft"; has "human   10"; has "open/ready"; has "20"; has "in-progress/review"
expect 0 "$TIX" stage ls; has "$wr  Workflow root  (3 stages)"
expect 0 "$TIX" new -t "Stage child" --parent "$wr" --status open/draft; wc=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 1 "$TIX" new -t "Bad stage" --parent "$wr" --status open/nope; has "not on this project's stage list"
expect 0 "$TIX" stage ls "$wc"; has "stages from ${wr} Workflow root"
expect 0 "$TIX" edit "$wc" --status open/ready
expect 1 "$TIX" edit "$wc" --status in-progress/ready; has "stage 'ready' belongs to open, not in-progress"
expect 2 "$TIX" edit "$wc" --status open/Ready; has "unknown status"
expect 0 "$TIX" show "$wc"; has "status:   open/ready"
expect 0 "$TIX" show "$wr"; has "stages:   open/draft*, open/ready, in-progress/review"
expect 0 "$TIX" ls --status open/ready; has "$wc  open/ready"; hasnt "$wr"
expect 0 "$TIX" ls --status open; has "$wc"; has "$wr"
expect 2 "$TIX" ls --status open/ready --status open/draft; has "only one stage"
wid=$("$TIX" show "$wc" | sed -n 's/^id: *//p')
out=$(git show "refs/tix/tickets/$wid:ops.ndjson")   # older readers still see the phase
[[ $out == *'{"op":"set-status","status":"open"}'$'\n''{"op":"set-state","status":"open","stage":"ready"}'* ]] ||
	fail "set-status must come right before set-state: $out"
expect 0 "$TIX" edit "$wc" --status done; expect 0 "$TIX" show "$wc"; has "status:   done"; hasnt "done/"
expect 1 "$TIX" stage rm "$wc" ready; has "no stage 'ready' on this ticket"
expect 0 "$TIX" stage rm "$wr" ready
expect 0 "$TIX" stage ls "$wr"; hasnt "open/ready"
expect 0 "$TIX" new -t "Free stage" --status blocked/whatever; has "Created ticket"   # no list: any stage

# ---- typed comments (§15.4) -------------------------------------------------------------
cd "$T/a"
expect 0 "$TIX" new -t "Needs answers"; tq=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" note "$tq" --kind question -m "Two things:" --question q1="Which database?" --question q2="Deadline?"
expect 0 "$TIX" show "$tq"; has "[question]"; has "    Two things:"; has "    ? q1: Which database?"
qc=$(grep -o '^--- [0-9a-f]*:[0-9]*' <<<"$out" | tail -1 | cut -c5-)
expect 0 "$TIX" note "$tq" --kind answer --reply-to "$qc" --field q1=Postgres --field q2="end of month"
expect 0 "$TIX" show "$tq"; has "[answer]  re $qc"; has "    q1: Postgres"; has "    q2: end of month"
expect 0 "$TIX" note "$tq" --kind review -m "Changes needed" --field verdict=changes --item f1="Add tests" --item f2="Fix typo=open"
rc=$(grep -o '^--- [0-9a-f]*:[0-9]*' <<<"$("$TIX" show "$tq")" | tail -1 | cut -c5-)
expect 0 "$TIX" show "$tq"; has "    verdict: changes"; has "    [open] f1: Add tests"; has "    [open] f2: Fix typo"
expect 0 "$TIX" note "$tq" --kind fix -m "done" --update "$rc#f1=addressed"
expect 0 "$TIX" show "$tq"; has "    [addressed] f1: Add tests"; has "    [open] f2: Fix typo"
expect 0 "$TIX" note "$tq" --kind raw --data '{"fields":{"a":"b"},"later":true}'
expect 0 "$TIX" show "$tq"; has "[raw]"; has "    a: b"
expect 2 "$TIX" note "$tq" --kind raw --data '[1]'; has "one JSON object"
expect 2 "$TIX" note "$tq" --kind raw --data '{}' --field a=b; has "use one of"
expect 2 "$TIX" note "$tq" --kind raw --item "-bad=x"; has "ids are"
expect 2 "$TIX" note "$tq" --field a=b; has "usage: tix note"
expect 1 "$TIX" note "$tq" --kind x --reply-to deadbee:2; has "no such comment"
expect 1 "$TIX" note "$tq" --kind Bad; has "comment kind"
echo '{"questions":[{"id":"q9","text":"from a file"}]}' > "$T/note.json"
expect 0 "$TIX" note "$tq" --kind question --data-file "$T/note.json"
expect 0 "$TIX" show "$tq"; has "    ? q9: from a file"

# ---- claims (§15.3): leases, claim --next, races through a remote ---------------------
git init -q --bare "$T/claims.git"
git init -q "$T/c1" && cd "$T/c1" && git remote add origin "$T/claims.git"
git config user.name Ada && git config user.email ada@example.org
expect 0 "$TIX" id new --name Ada --kind agent
expect 0 "$TIX" new -t "First job"; j1=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" new -t "Second job" --dep "+$j1"; j2=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" new -t "Third job" --rank last; j3=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" edit "$j1" --rank first
expect 0 "$TIX" sync
git clone -q "$T/claims.git" "$T/c2" 2>/dev/null && cd "$T/c2"
git config user.name Bob && git config user.email bob@example.org
expect 0 "$TIX" pull
expect 0 "$TIX" id new --name Bob --kind agent
cd "$T/c1"
expect 0 "$TIX" claim --next; has "Claimed $j1 First job (for 30 min)"
expect 0 "$TIX" show "$j1"; has "claimed:  Ada until"
expect 0 "$TIX" ls; has "First job  @Ada"
cd "$T/c2"
expect 0 "$TIX" claim --next --for 2h; has "Claimed $j3 Third job (for 120 min)"   # j1 taken, j2 waits on j1
expect 0 "$TIX" claim --next; has "Claimed $j3"   # what you hold comes back first (renewed)
expect 3 "$TIX" claim --next --status blocked; has "no work"
expect 1 "$TIX" claim "$j1"; has "claimed by"
expect 1 "$TIX" claim "$j1" --release; has "only they can release it"
expect 1 "$TIX" claim "$j2" --release; has "isn't claimed"
expect 2 "$TIX" claim "$j2" --for 3d; has "up to 24h"
expect 2 "$TIX" claim --next --status wip; has "unknown status"
expect 0 "$TIX" claim --next --local; has "Claimed $j3"   # my own comes back (renewed)
# a race: Bob claims j2 locally without pushing, Ada finishes j1 and claims j2 through the remote
cd "$T/c1"
expect 0 "$TIX" edit "$j1" --status done
expect 0 "$TIX" claim "$j1" --release
expect 0 "$TIX" sync
cd "$T/c2"
expect 0 "$TIX" pull
expect 0 "$TIX" claim "$j2" --for 1h   # not pushed
cd "$T/c1"
expect 0 "$TIX" claim --next; has "Claimed $j2"
cd "$T/c2"
expect 0 "$TIX" claim --next; has "Claimed $j3"   # Bob's pull shows Ada's claim on j2: he yields it
expect 0 "$TIX" show "$j2"; has "claimed:  Ada until"; hasnt "waiting:"
cd "$T/c1" && expect 0 "$TIX" pull && expect 0 "$TIX" show "$j2"; has "claimed:  Ada until"; hasnt "waiting:"
expect 0 "$TIX" claim "$j2" --release; has "Released"
expect 0 "$TIX" show "$j2"; hasnt "claimed:"

# ---- autonomy (§15.5): the switch, expiry, the guard, halt -----------------------------
cd "$T/c1"   # Ada is an agent here (claims section)
expect 0 "$TIX" new -t "Night root"; nr=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" stage add "$nr" needs-input --phase open --human
expect 0 "$TIX" stage add "$nr" ready --phase open
expect 0 "$TIX" stage add "$nr" aborted --phase blocked --human
expect 0 "$TIX" new -t "Night job" --parent "$nr" --status open/ready; nj=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" autonomy "$nj" show; has "supervised (nothing up the tree"
expect 1 "$TIX" autonomy "$nr" on --for 8h; has "agents may only halt"
expect 0 "$TIX" id edit --kind human
expect 2 "$TIX" autonomy "$nr" on; has "needs an end"
expect 2 "$TIX" autonomy "$nr" on --for 8h --no-expiry; has "needs an end"
expect 2 "$TIX" autonomy "$nr" on --until 25:00; has "--until is HH:MM"
expect 2 "$TIX" autonomy "$nr" maybe; has "on, off, halt or show"
expect 0 "$TIX" autonomy "$nr" on --for 8h; has "autonomous until"
expect 0 "$TIX" autonomy "$nj" show; has "autonomous until"; has "(set on ${nr}"
expect 0 "$TIX" show "$nr"; has "autonomy: autonomous until"
expect 0 "$TIX" edit "$nj" --status open/needs-input   # a person isn't guarded
expect 0 "$TIX" edit "$nj" --status open/ready
expect 0 "$TIX" id edit --kind agent
expect 1 "$TIX" edit "$nj" --status open/needs-input; has "autonomous mode: 'needs-input' waits for a person"
expect 0 "$TIX" edit "$nj" --status blocked/aborted   # the escape hatch
expect 0 "$TIX" edit "$nj" --status open/ready
expect 0 "$TIX" autonomy "$nr" halt; has "halted"
expect 3 "$TIX" claim --next --status open/ready --local; has "no work"
expect 1 "$TIX" autonomy "$nr" off; has "agents may only halt"
expect 0 "$TIX" id edit --kind human
expect 0 "$TIX" autonomy "$nr" on --until "2099-01-01 08:00"; has "autonomous until 2099-01-01"
expect 0 "$TIX" autonomy "$nr" on --no-expiry; has "autonomous  (set on"
expect 0 "$TIX" autonomy "$nr" off; has "supervised"
expect 0 "$TIX" id edit --kind agent
expect 0 "$TIX" edit "$nj" --status open/needs-input   # supervised again: fine

# ---- agent interfaces (§15.6): --json, log, watch -----------------------------------------
cd "$T/c1"
expect 0 "$TIX" ls --json --all; has '"short":"'"$nj"'"'; has '"status":"open","stage":"needs-input"'
[ "$(grep -c '^{' <<<"$out")" = "$(wc -l <<<"$out")" ] || fail "ls --json: one object per line"
expect 0 "$TIX" show "$nj" --json; has '"type":"ticket"'; has '"stage":"needs-input"'; has '"waiting":[]'
expect 0 "$TIX" show --json "$nj"; has '"title":"Night job"'
expect 0 "$TIX" stage ls "$nj" --json; has '"stages":[{"name":"needs-input","phase":"open","human":true'
expect 2 "$TIX" stage ls --json; has "--json needs a TICKET"
expect 0 "$TIX" autonomy "$nj" show --json; has '"mode":"supervised","until":null,"owner":"'
expect 0 "$TIX" edit "$nj" --status open/ready
expect 0 "$TIX" claim --next --json --local --status open/ready; has '"short":"'"$nj"'"'; has '"live":true'
expect 3 "$TIX" claim --next --json --local --status blocked; has "null"
expect 0 "$TIX" log "$nj"; has "Night job: created: Night job"; has "status open/ready"; has "claimed"
expect 0 "$TIX" log "$nj" --json --limit 1; has '"op":"claim"'; has '"summary":"claimed"'
[ "$(wc -l <<<"$out")" = 1 ] || fail "log --limit 1 printed more than one line"
expect 0 "$TIX" log --since 1h --under "$nr"; has "Night root"; has "Night job"
expect 2 "$TIX" log --since yesterday; has "--since is like"
expect 2 "$TIX" watch --pull 1s; has "at least 5s"
timeout 20 "$TIX" watch --json --max-events 1 > "$T/watch.out" 2>&1 & wpid=$!
sleep 1.5
"$TIX" note "$nj" --kind question -m "Port 80 or 8080?" >/dev/null
wait $wpid; wrc=$?
out=$(cat "$T/watch.out")
[ $wrc = 0 ] || fail "watch exited $wrc: $out"
has '"short":"'"$nj"'"'; has '"op":"typed-comment"'; has '"summary":"[question] Port 80 or 8080?"'
expect 0 "$TIX" new -t "Other root"; orr=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" new -t "Other job" --parent "$orr"; oj=$(sed -n 's/^Created ticket //p' <<<"$out")
timeout 20 "$TIX" watch --under "$orr" --max-events 1 > "$T/watch2.out" 2>&1 & wpid=$!
sleep 1.5
"$TIX" comment "$nj" -m "not under that root" >/dev/null   # filtered out
sleep 1.5
"$TIX" comment "$oj" -m "under it" >/dev/null
wait $wpid; wrc=$?
out=$(cat "$T/watch2.out")
[ $wrc = 0 ] || fail "watch --under exited $wrc: $out"
has "Other job: comment: under it"; hasnt "not under that root"

# ---- index cache (M5) ------------------------------------------------------------
cd "$T/a"
expect 0 "$TIX" index clear; has "index: 0 cached"
expect 0 "$TIX" ls --all; cached=$out
expect 0 "$TIX" index status; has "cached entit"; hasnt "index: 0 "
expect 0 "$TIX" ls --all; [ "$out" = "$cached" ] || fail "cached listing differs"
TIX_NO_INDEX=1 expect 0 "$TIX" ls --all; [ "$out" = "$cached" ] || fail "uncached listing differs"
TIX_NO_INDEX=1 expect 0 "$TIX" index status; has "index: off"
# a ref moved behind tix's back (plain git) is re-validated, not served stale
before=$(git rev-parse "refs/tix/tickets/$(git for-each-ref --format='%(refname:lstrip=3)' "refs/tix/tickets/$t2*")")
expect 0 "$TIX" edit "$t2" --title "Second ticket, renamed"
after=$(git rev-parse "refs/tix/tickets/$(git for-each-ref --format='%(refname:lstrip=3)' "refs/tix/tickets/$t2*")")
expect 0 "$TIX" show "$t2"; has "Second ticket, renamed"
git update-ref "refs/tix/tickets/$(git for-each-ref --format='%(refname:lstrip=3)' "refs/tix/tickets/$t2*")" "$before" "$after"
expect 0 "$TIX" show "$t2"; hasnt "renamed"; has "Second ticket"
# a corrupt cache file is thrown away and rebuilt
rm -f .git/tix/index.db-wal .git/tix/index.db-shm
printf 'garbage, not a database at all, garbage garbage garbage garbage' > .git/tix/index.db
expect 0 "$TIX" ls --all; [ "$out" = "$cached" ] || fail "listing after cache corruption differs"
expect 0 "$TIX" index status; hasnt "index: off"
# a symlink in its place is refused, and tix carries on without a cache
rm -f .git/tix/index.db .git/tix/index.db-wal .git/tix/index.db-shm
ln -s /dev/null .git/tix/index.db
expect 0 "$TIX" ls --all; [ "$out" = "$cached" ] || fail "listing with a symlinked cache differs"
expect 0 "$TIX" index status; has "index: off"
rm -f .git/tix/index.db

# SHA-256 + reftable repository
git init -q --object-format=sha256 --ref-format=reftable "$T/s" && cd "$T/s"
expect 0 "$TIX" id new --name S --email s@example.org
expect 0 "$TIX" new -t "sha256 ticket"
t=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" show "$t"; has "sha256 ticket"
[ "$(sed -n 's/^id: *//p' <<<"$out" | tr -d '\n' | wc -c)" = 64 ] || fail "sha256 id length"
git fsck --strict --no-dangling --no-progress >/dev/null 2>&1 || fail "git fsck sha256"
git init -q --bare --object-format=sha256 "$T/s.git" && git remote add origin "$T/s.git"
expect 0 "$TIX" push; has "pushed 2"
git clone -q "$T/s.git" "$T/s2" 2>/dev/null && cd "$T/s2"
expect 0 "$TIX" pull; has "2 new"
expect 0 "$TIX" show "$t"; has "sha256 ticket"

cd "$T/s"
# new --status records finished work in one step; ls sorts active first, then WBS order
expect 0 "$TIX" new -t "Already shipped" --status done --actual-start 2026-01-05 --actual-finish 2026-01-06
t3=$(sed -n 's/^Created ticket //p' <<<"$out")
expect 0 "$TIX" show "$t3"; has "status:   done"
expect 0 "$TIX" edit "$t" --status in-progress
expect 0 "$TIX" ls --all; first=$(head -1 <<<"$out"); [[ $first == *"in-progress"* ]] || fail "ls: in-progress not first: $out"
expect 0 "$TIX" ls --all --sort newest; has "$t3"   # order itself: core test_sort (times here share a second)

# ---- baselines: take, compare, export, sync ------------------------------------------
git init -q "$T/bl" && cd "$T/bl"
export TIX_TODAY=2026-11-09
mk() { sed -n 's/^Created [a-z]* //p' <<<"$out"; }
expect 0 "$TIX" id new --name Ada --email ada@example.org
expect 0 "$TIX" res new Dev --rate "100.00 EUR"; dev=$(mk)
expect 0 "$TIX" new -t Proj; proj=$(mk)
expect 0 "$TIX" new -t Build --parent "$proj" --rank last --duration 3d; build=$(mk)
expect 0 "$TIX" assign "$build" "$dev"
expect 0 "$TIX" new -t Ship --parent "$proj" --rank last --duration 1d --dep "+$build"; ship=$(mk)
expect 2 "$TIX" baseline; has "usage: tix baseline"
expect 0 "$TIX" baseline take "Plan of record"; has 'Took baseline'; has '"Plan of record": 3 tasks'
bl=$(sed -n 's/^Took baseline \([0-9a-f]*\) .*/\1/p' <<<"$out")
expect 0 "$TIX" baseline take "Build only" "$build"; has ": 1 task"
expect 0 "$TIX" baseline ls; has "$bl  2026-11-09  Plan of record  (3 tasks)"; has "Build only  (1 task, from $build)"
expect 0 "$TIX" baseline show "$bl"; has "taken:    2026-11-09"; has "$build  2026-11-09..2026-11-11      3        -     2400.00 EUR"
expect 0 "$TIX" baseline rename "$bl" "Approved"; expect 0 "$TIX" baseline ls; has "Approved"
expect 0 "$TIX" edit "$build" --duration 5d
expect 0 "$TIX" new -t Docs --parent "$proj" --rank last --duration 1d
expect 0 "$TIX" plan --baseline "$bl"; has "BASE FIN    SLIP"
has "Build                         2026-11-09  2026-11-13      5      0  2026-11-11    +2  critical"
has "Ship                          2026-11-16  2026-11-16      1      0  2026-11-12    +2"; has "  new"
has 'baseline "Approved" (2026-11-09): 3 finish later, 0 earlier, 1 new, 0 removed since'
expect 0 "$TIX" wbs --baseline "$bl"; has "slip +2"; has "slip new"
expect 0 "$TIX" export --format csv --baseline "$bl"; has "Dev:100%,4000.00,EUR,2026-11-09,2026-11-11,0,2,2,,1600.00"
expect 0 "$TIX" export --format json --baseline "$bl"; has '"baseline":{"start":"2026-11-09","finish":"2026-11-11"'
expect 0 "$TIX" export --format mspdi --baseline "$bl"; has "<Baseline><Number>0</Number>"; has "<Cost>400000</Cost>"
expect 1 "$TIX" plan --baseline "$build"; has "no baseline"
git init -q --bare "$T/bl.git" && git remote add origin "$T/bl.git"
expect 0 "$TIX" push
git clone -q "$T/bl.git" "$T/bl2" 2>/dev/null && cd "$T/bl2"
expect 0 "$TIX" pull; expect 0 "$TIX" baseline ls; has "Approved"; has "Build only"
expect 0 "$TIX" baseline show "$bl"; has "2400.00 EUR"

# ---- issue-tracker bridge: arguments and token lookup (the sync itself: modules/bridge tests,
#      the adapter against a fake Forgejo, and a real Forgejo check, see architecture §14.4)
cd "$T/bl"
expect 2 "$TIX" bridge; has "usage: tix bridge"
expect 2 "$TIX" bridge pull; has "give --url"
expect 2 "$TIX" bridge pull --url http://example.org/me/proj; has "is not https://host/owner/repo"
expect 1 env -u TIX_FORGEJO_TOKEN "$TIX" bridge pull --url https://git.example.invalid/me/proj; has "no token for git.example.invalid"
expect 1 env TIX_FORGEJO_TOKEN=x "$TIX" bridge pull --url https://git.example.invalid/me/proj --dry-run; has "forgejo: GET"

# ---- commit signatures (ssh): sign, audit, require on pull, trust on first use ----------
if command -v ssh-keygen >/dev/null; then
	git init -q "$T/sg" && cd "$T/sg"
	ssh-keygen -q -t ed25519 -N "" -C ada -f "$T/ada_key"
	git config gpg.format ssh && git config user.signingkey "$T/ada_key.pub" && git config tix.sign true
	expect 0 "$TIX" id new --name Ada --email ada@example.org
	expect 0 "$TIX" id edit --add-signing-key; has "Adding ssh key SHA256:"
	expect 0 "$TIX" id show; has "key:    ssh SHA256:"
	expect 0 "$TIX" new -t "Signed work"; st=$(sed -n 's/^Created ticket //p' <<<"$out")
	git cat-file -p "refs/tix/tickets/$(git for-each-ref --format='%(refname:lstrip=3)' "refs/tix/tickets/$st*")" |
		grep -q '^gpgsig ' || fail "ticket commit not signed"
	expect 0 "$TIX" verify; has "0 unsigned, 0 bad"; hasnt "ticket "
	git init -q --bare "$T/sg.git" && git remote add origin "$T/sg.git"
	expect 0 "$TIX" push
	git clone -q "$T/sg.git" "$T/sg2" 2>/dev/null && cd "$T/sg2" && git config tix.verify require
	expect 0 "$TIX" pull; has "2 new"
	cd "$T/sg" && git config tix.sign false
	expect 0 "$TIX" edit "$st" --title "Unsigned edit"
	expect 0 "$TIX" push
	expect 0 "$TIX" verify; has "1 unsigned"; has "ticket   $st"   # policy off: reported, not an error
	git config tix.verify require
	expect 1 "$TIX" verify; has "1 unsigned"
	cd "$T/sg2"
	expect 1 "$TIX" pull; has "rejected refs/tix-remotes/origin/tickets/"; has "1 unsigned and 0 badly signed"
	expect 0 "$TIX" show "$st"; has "Signed work"; hasnt "Unsigned edit"
	expect 2 "$TIX" pull --verify sometimes; has "--verify is off, warn or require"
	git clone -q "$T/sg.git" "$T/sg3" 2>/dev/null && cd "$T/sg3" && git config tix.verify require
	expect 1 "$TIX" pull; has "rejected"
	expect 1 "$TIX" pull --verify warn; has "warning: refs/tix-remotes/origin/tickets/"   # trust on first use
	expect 0 "$TIX" show "$st"; has "Unsigned edit"
	expect 0 "$TIX" pull; has "nothing new"   # require again, nothing new to check
fi

if [ $fails -gt 0 ]; then echo "$fails failure(s)"; exit 1; fi
echo "cli end-to-end OK"
