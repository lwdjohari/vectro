#!/usr/bin/env bash
set -euo pipefail

# Usage: ./bin/submodules-init.sh [--deep] [--no-sync]
#   --deep     : disable shallow recommendation (full history)
#   --no-sync  : skip 'git submodule sync' (rarely needed)

RED=$'\e[31m'; GRN=$'\e[32m'; YLW=$'\e[33m'; CLR=$'\e[0m'

if [ ! -f .gitmodules ]; then
  echo "${YLW}[submodules] No .gitmodules found — nothing to init.${CLR}"
  exit 0
fi

JOBS=${JOBS:-8}
OPT_DEEP=0
OPT_SYNC=1
for a in "$@"; do
  case "$a" in
    --deep) OPT_DEEP=1 ;;
    --no-sync) OPT_SYNC=0 ;;
    *) echo "${YLW}[submodules] Unknown arg: $a${CLR}" ;;
  esac
done

if ! git rev-parse --git-dir >/dev/null 2>&1; then
  echo "${RED}[submodules] Not a git repo.${CLR}"; exit 1
fi

# Warn if worktree is dirty (submodule update may reset nested changes)
if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "${YLW}[submodules] Working tree has local changes. Proceeding anyway…${CLR}"
fi

# Make sure submodule URLs/branches match .gitmodules
if [ $OPT_SYNC -eq 1 ]; then
  echo "${GRN}[submodules] Syncing URLs from .gitmodules…${CLR}"
  git submodule sync --recursive
fi

# Decide shallow vs deep
RECURSE_ARGS=(update --init --recursive --jobs "$JOBS")
if [ $OPT_DEEP -eq 0 ]; then
  # Prefer shallow when possible (faster clones in CI/containers)
  GIT_VERSION=$(git version | awk '{print $3}')
  # --recommend-shallow is safe to pass on modern versions; older Git ignores unknown opts
  RECURSE_ARGS+=(--recommend-shallow)
fi

echo "${GRN}[submodules] Populating modules…${CLR}"
git submodule "${RECURSE_ARGS[@]}"

# Optional: ensure submodules are on their configured branch (not detached)
# Requires Git 2.22+: 'git submodule set-branch' (preconfigured in .gitmodules)
if grep -q "branch =" .gitmodules 2>/dev/null; then
  echo "${GRN}[submodules] Checking out configured branches…${CLR}"
  git submodule foreach --recursive '
    b=$(git config -f $toplevel/.gitmodules submodule.$name.branch || true);
    if [ -n "$b" ]; then
      # create local tracking if needed
      git fetch --tags --force origin "+refs/heads/$b:refs/remotes/origin/$b" || true
      git checkout -B "$b" "origin/$b" || true
    fi
  '
fi

echo "${GRN}[submodules] Done.${CLR}"
