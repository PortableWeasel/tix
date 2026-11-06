#!/usr/bin/env bash
# Runs the store contract against a fresh repository of the given format, then
# checks the result with the real git binary.
set -euo pipefail
BIN=$(realpath "$1"); FMT=$2
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=tix GIT_AUTHOR_EMAIL=tix@example.invalid
export GIT_COMMITTER_NAME=tix GIT_COMMITTER_EMAIL=tix@example.invalid
case $FMT in
  sha1-files) git init -q "$T/r" ;;
  sha256-reftable) git init -q --object-format=sha256 --ref-format=reftable "$T/r" ;;
esac
git init -q --bare $( [ "$FMT" = sha256-reftable ] && echo --object-format=sha256 ) "$T/peer.git"
cd "$T/r"
git remote add peer "$T/peer.git"
mkdir sub && echo a > a && echo b > sub/b && git add a sub && git commit -qm normal
blob=$(git rev-parse HEAD:a)
cd sub   # discovery from a subdirectory
"$BIN" "$(git rev-parse HEAD)" "$blob"
cd ..
git fsck --strict --no-dangling --no-progress >/dev/null 2>&1 || { echo "FAIL: fsck"; exit 1; }
[ "$(git ls-tree --name-only refs/tix/contract/x)" = ops.ndjson ] || { echo "FAIL: tree layout"; exit 1; }
[ "$(git cat-file -p refs/tix/contract/x:ops.ndjson)" = b ] || { echo "FAIL: blob content"; exit 1; }
[ "$(cat "$(git rev-parse --git-dir)/tix/contract-v")" = 7 ] || { echo "FAIL: local state file"; exit 1; }
[ "$(git config --local tix.contract)" = yes ] || { echo "FAIL: config"; exit 1; }
[ "$(git --git-dir="$T/peer.git" cat-file -p refs/tix/contract-sync/x:ops.ndjson)" = "sync b" ] ||
  { echo "FAIL: pushed ref on the remote"; exit 1; }
git --git-dir="$T/peer.git" fsck --strict --no-dangling --no-progress >/dev/null 2>&1 || { echo "FAIL: remote fsck"; exit 1; }
echo "store_libgit contract OK ($FMT)"
