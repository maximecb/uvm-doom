#!/bin/sh
# Build the PureDOOM/UVM front-end with uvclang and run it on the UVM VM.
#
#   ./build_and_run.sh [args...]
#
# Any extra arguments are forwarded to UVM DOOM (e.g. ./build_and_run.sh -timedemo).
#
# Steps:
#   1. Fetch the uvm git submodule if it isn't checked out yet.
#   2. Compile main.c -> out.asm at -O2 (skipped if out.asm already exists;
#      delete out.asm to force a rebuild).
#   3. Run the result on the UVM VM, built in release mode.
set -e

# Run from the repo root (where this script, main.c and doom1.wad live), so the
# quoted #include "PureDOOM.h" and the game's doom1.wad resolve relative to it.
cd "$(dirname "$0")"

# 1. Make sure the uvm submodule is fetched. Only initialize when it's missing:
#    a plain `submodule update` would reset it to the commit pinned by the
#    superproject, which we don't want to force here.
if [ ! -f uvm/uvclang/Cargo.toml ]; then
    echo "Fetching uvm submodule..."
    git submodule update --init --recursive
fi

# 2. Compile main.c -> out.asm at -O2, unless it's already built.
if [ -f out.asm ]; then
    echo "out.asm already built; skipping compile (delete it to rebuild)."
else
    echo "Compiling main.c -> out.asm (-O2)..."
    cargo run --release --manifest-path uvm/uvclang/Cargo.toml -- -O2 main.c -o out.asm
fi

# 3. Run UVM DOOM on the VM (release build).
echo "Launching UVM DOOM..."
RUST_BACKTRACE=1 exec cargo run --release --manifest-path uvm/vm/Cargo.toml -- out.asm "$@"
