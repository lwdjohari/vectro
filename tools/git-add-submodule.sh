#!/usr/bin/env bash
#
# --------------------------------------------------------------------
# add-submodule.sh — Safe helper to add and pin shallow Git submodules
# --------------------------------------------------------------------
# This script simplifies adding third-party dependencies (from HTTPS or SSH)
# as Git submodules pinned to a specific tag.
#
# ✅ What it does:
#   1. Adds the given repository as a Git submodule at the specified path.
#   2. Fetches *only* the given tag shallowly (depth=1) for minimal history.
#   3. Checks out that tag (detached HEAD).
#   4. Marks the submodule as shallow in `.gitmodules` so future updates stay thin.
#   5. Stages `.gitmodules` and the submodule path for commit.
#
# 💡 Use this when:
#   - You want to lock a dependency (e.g. a header-only library) to a fixed version tag.
#   - You don’t want to clone the full history (saves space & time).
#   - You need a reproducible, pinned, shallow submodule setup.
#
# Example:
#   ./bin/add-submodule.sh https://github.com/cameron314/concurrentqueue.git thirdparty/concurrentqueue v1.0.4
#
# Result:
#   - thirdparty/concurrentqueue/ checked out at tag v1.0.4
#   - .gitmodules updated with `shallow = true`
#   - Ready to commit with `git commit -m "Add concurrentqueue v1.0.4 (shallow)"`
#
# --------------------------------------------------------------------

set -Eeuo pipefail

if [[ $# -lt 3 ]]; then
  echo "Usage: $0 <repo-url> <path> <tag>"
  echo
  echo "Adds a shallow Git submodule pinned to a specific tag."
  echo
  echo "Example:"
  echo "  $0 https://github.com/cameron314/concurrentqueue.git thirdparty/concurrentqueue v1.0.4"
  echo
  echo "Steps performed:"
  echo "  1. Adds the submodule if missing"
  echo "  2. Fetches the specified tag shallowly"
  echo "  3. Checks out that tag (detached HEAD)"
  echo "  4. Marks submodule as shallow in .gitmodules"
  echo "  5. Stages .gitmodules and submodule for commit"
  exit 1
fi

REPO_URL="$1"
TARGET_PATH="$2"
TAG="$3"

echo "==> Adding shallow submodule"
echo "Repo: ${REPO_URL}"
echo "Path: ${TARGET_PATH}"
echo "Tag:  ${TAG}"
echo

# Step 1: Add submodule if missing
if [[ -d "${TARGET_PATH}" ]]; then
  echo "Submodule path already exists: ${TARGET_PATH}"
else
  git submodule add "${REPO_URL}" "${TARGET_PATH}"
fi

# Step 2: Fetch only the target tag shallowly
echo "==> Fetching tag '${TAG}' shallowly..."
git -C "${TARGET_PATH}" fetch --depth 1 origin tag "${TAG}"

# Step 3: Checkout the tag
echo "==> Checking out tag '${TAG}'..."
git -C "${TARGET_PATH}" checkout "${TAG}"

# Step 4: Mark as shallow for future updates
echo "==> Marking submodule as shallow in .gitmodules..."
git config -f .gitmodules submodule."${TARGET_PATH}".shallow true

# Step 5: Stage and remind user to commit
git add .gitmodules "${TARGET_PATH}"
echo
echo "✅ Submodule added and pinned to tag '${TAG}'"
echo "   Commit with:"
echo "   git commit -m \"Add $(basename ${TARGET_PATH}) ${TAG} (shallow)\""
