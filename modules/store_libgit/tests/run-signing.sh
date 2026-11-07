#!/usr/bin/env bash
# Signed commits with real keys: ssh (ssh-keygen) and openpgp (gpg in a throwaway home).
# Then a tampered copy of a signed commit must not verify. argv[1] = test binary, argv[2] = ssh|gpg.
set -euo pipefail
BIN=$(realpath "$1"); KIND=$2
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=tix GIT_AUTHOR_EMAIL=tix@example.invalid
export GIT_COMMITTER_NAME=tix GIT_COMMITTER_EMAIL=tix@example.invalid
git init -q "$T/r" && cd "$T/r"
git config tix.sign true
unsigned=$(git commit-tree "$(git mktree </dev/null)" -m plain)
if [ "$KIND" = ssh ]; then
  command -v ssh-keygen >/dev/null || { echo "no ssh-keygen: skipped"; exit 77; }
  ssh-keygen -q -t ed25519 -N "" -C me -f "$T/me"
  ssh-keygen -q -t ed25519 -N "" -C other -f "$T/other"
  git config gpg.format ssh
  git config user.signingkey "$T/me.pub"
  other_fpr=$(ssh-keygen -l -E sha256 -f "$T/other.pub" | cut -d' ' -f2)
  other_pub=$(cut -d' ' -f1-2 "$T/other.pub")
else
  command -v gpg >/dev/null || { echo "no gpg: skipped"; exit 77; }
  export GNUPGHOME="$T/gnupg"; mkdir -m 700 "$GNUPGHOME"
  gpg --batch --quiet --passphrase '' --quick-gen-key "Me <me@example.invalid>" ed25519 sign never 2>/dev/null
  gpg --batch --quiet --passphrase '' --quick-gen-key "Other <other@example.invalid>" ed25519 sign never 2>/dev/null
  me_fpr=$(gpg --batch --with-colons --fingerprint me@example.invalid | awk -F: '/^fpr/ {print $10; exit}')
  other_fpr=$(gpg --batch --with-colons --fingerprint other@example.invalid | awk -F: '/^fpr/ {print $10; exit}')
  other_pub=$(gpg --batch --armor --export "$other_fpr")
  git config gpg.format openpgp
  git config user.signingkey "$me_fpr"
fi
signed=$("$BIN" "$unsigned" "$KIND" "$other_fpr" "$other_pub" | tail -1)
# git itself agrees the commit is signed
git cat-file -p "$signed" | grep -q '^gpgsig ' || { echo "FAIL: no gpgsig header"; exit 1; }
# same signature, different message: a forgery that must never verify
tampered=$(git cat-file commit "$signed" | sed 's/^a signed pack$/a forged pack/' | git hash-object -t commit -w --stdin)
[ "$tampered" != "$signed" ] || { echo "FAIL: tampering changed nothing"; exit 1; }
"$BIN" tampered "$tampered"
echo "store_libgit signing OK ($KIND)"
