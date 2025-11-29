#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 path/to/submodule"
  exit 1
fi

sub_path="$1"

# Ensure we're in a git repo
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Error: not inside a git repository."
  exit 1
fi

# Ensure the path exists
if [[ ! -e "$sub_path" ]]; then
  echo "Error: path '$sub_path' does not exist."
  exit 1
fi

# Ensure it's actually a submodule (gitlink, mode 160000)
if ! git ls-files --stage -- "$sub_path" | awk '{print $1}' | grep -q '^160000$'; then
  echo "Error: '$sub_path' is not a git submodule (gitlink in index)."
  exit 1
fi

# Ensure .gitmodules exists
if [[ ! -f .gitmodules ]]; then
  echo "Error: .gitmodules file not found in repo root."
  exit 1
fi

# Derive submodule name from .gitmodules using the path
sub_name="$(
  git config -f .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null \
    | awk -v p="$sub_path" '
        $2 == p {
          name = $1
          sub(/^submodule\./, "", name)
          sub(/\.path$/, "", name)
          print name
        }
      '
)"

if [[ -z "${sub_name:-}" ]]; then
  echo "Error: no submodule entry in .gitmodules for path '$sub_path'."
  exit 1
fi

echo "About to remove submodule:"
echo "  name : $sub_name"
echo "  path : $sub_path"
echo
read -r -p "Proceed with removal? [y/N] " ans
case "$ans" in
  y|Y|yes|YES) ;;
  *)
    echo "Aborted by user."
    exit 1
    ;;
esac

echo "Deinitializing submodule..."
git submodule deinit -f -- "$sub_path"

echo "Removing submodule from index and working tree..."
git rm -f "$sub_path"

echo "Cleaning .gitmodules entry (if any still present)..."
if git config -f .gitmodules --get "submodule.$sub_name.path" >/dev/null 2>&1; then
  git config -f .gitmodules --remove-section "submodule.$sub_name" || true
fi

if [[ -f .gitmodules ]]; then
  if grep -q '\S' .gitmodules; then
    # .gitmodules still has content
    git add .gitmodules
  else
    echo "Removing empty .gitmodules file..."
    rm .gitmodules
    # Stage its removal if tracked
    git add -u .gitmodules || true
  fi
fi

modules_dir=".git/modules/$sub_name"
if [[ -d "$modules_dir" ]]; then
  echo "Removing leftover submodule data at $modules_dir..."
  rm -rf "$modules_dir"
fi

echo
echo "Submodule '$sub_name' at '$sub_path' removed."
echo "Review changes with: git status"
echo
echo "Suggested commit command:"
echo "  git commit -m \"Remove submodule $sub_name\""
