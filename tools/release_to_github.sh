#!/usr/bin/env bash
#
# release_to_github.sh — Build a clean public release of Quilla and optionally
# push it, WITHOUT importing the private development history.
#
# How it works
# ------------
# The branch `github-release` holds only intentional public release commits.
# Each run overwrites its tree with the current `main` tree (tree copy only,
# no history merge) and commits it as a new release. Internal directories
# excluded by .gitignore (safety/, .workbuddy/, .githooks/, OPEN-SOURCE-PREP.md)
# never enter the public repo.
#
# The local pre-push hook forbids pushing to `main`, so we push to `develop`.
#
# Usage
# -----
#   ./tools/release_to_github.sh <version> [--push]
#     <version>   release tag, e.g. v0.11
#     --push      also push github-release -> origin/develop (off by default)
#
# Examples
# --------
#   ./tools/release_to_github.sh v0.11        # prepare + commit, review, no push
#   ./tools/release_to_github.sh v0.11 --push # prepare + commit + push
#
set -euo pipefail

RELEASE_BRANCH="github-release"
SOURCE_BRANCH="main"
REMOTE="origin"
PUSH_TARGET="develop"

VERSION="${1:-}"
PUSH=0
if [[ "${2:-}" == "--push" ]]; then
  PUSH=1
fi

if [[ -z "$VERSION" ]]; then
  echo "usage: $0 <version> [--push]" >&2
  echo "  example: $0 v0.11" >&2
  exit 2
fi

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

# Refuse on a dirty tree: switching branches would lose or confuse changes.
if [[ -n "$(git status --porcelain)" ]]; then
  echo "error: working tree is not clean. Commit or stash changes first:" >&2
  git status --short >&2
  exit 1
fi

if ! git rev-parse --verify "$SOURCE_BRANCH" >/dev/null 2>&1; then
  echo "error: source branch '$SOURCE_BRANCH' not found." >&2
  exit 1
fi

echo "==> switch to $RELEASE_BRANCH"
git checkout -q "$RELEASE_BRANCH"

echo "==> reset index, then copy $SOURCE_BRANCH tree (no history)"
git rm -rf --cached -q .
git checkout -q "$SOURCE_BRANCH" -- .
# drop any file present on disk but not in $SOURCE_BRANCH (untracked after copy)
git clean -fdq

echo "==> stage (respecting .gitignore)"
git add -A

if git diff --cached --quiet; then
  echo "error: no changes vs the previous release; nothing to do." >&2
  exit 1
fi

echo "==> commit as release $VERSION"
git commit -q -m "Quilla $VERSION - public release"

if [[ "$PUSH" -eq 1 ]]; then
  echo "==> push $RELEASE_BRANCH -> $REMOTE/$PUSH_TARGET"
  git push -u "$REMOTE" "$RELEASE_BRANCH:$PUSH_TARGET"
else
  echo "==> prepared (not pushed). Review with: git show --stat"
  echo "    To push later: git push -u $REMOTE $RELEASE_BRANCH:$PUSH_TARGET"
fi
