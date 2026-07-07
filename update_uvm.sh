#!/bin/sh
# Update the uvm submodule to the latest commit on origin/main
# and stage the change in the parent repository.
set -e

# Move to the directory containing this script (the repo root).
cd "$(dirname "$0")"

echo "Fetching latest uvm from origin/main..."
git -C uvm fetch origin main

echo "Checking out origin/main..."
git -C uvm checkout origin/main

new_commit=$(git -C uvm rev-parse --short HEAD)
echo "uvm is now at $new_commit"

# Stage the updated submodule pointer in the parent repo.
git add uvm

echo "Done. Review the change with 'git diff --cached' and commit when ready."
