#!/bin/sh
# Benchmark UVM DOOM by running its longest built-in demo under -timedemo.
#
#   ./timedemo.sh
#
# DEMO1 is the longest of the three demos in doom1.wad (20118 bytes, vs 15358
# for DEMO2 and 8550 for DEMO3), so it gives the most stable timing sample.
#
# -timedemo replays the demo as fast as possible (DOOM's singletics mode, no
# frame pacing) with no window, exercising the full per-frame pipeline: game
# logic, DOOM's software renderer, and our 3x nearest-neighbor upscale/byte-swap
# (vm_present_frame). Only the window blit and audio are skipped. On completion
# it prints two lines:
#
#   Error: timed <gametics> gametics in <realtics> realtics
#   [uvm-doom] benchmark: <frames> frames in <ms> ms = <fps> fps
#
# The first is DOOM's own report (realtics are 1/35s units; "Error:" is just how
# DOOM prints it, not a failure). The second is a wall-clock frames/sec figure
# measured over the whole run. Expect this to take a couple of minutes: on the
# interpreted VM the upscale dominates, so the frame rate is modest.
set -e

cd "$(dirname "$0")"

exec ./build_and_run.sh -timedemo demo1
