#!/bin/sh
# Benchmark UVM DOOM by running its longest built-in demo under -timedemo.
#
#   ./timedemo.sh
#
# DEMO1 is the longest of the three demos in doom1.wad (20118 bytes, vs 15358
# for DEMO2 and 8550 for DEMO3), so it gives the most stable timing sample.
#
# -timedemo replays the demo as fast as possible (DOOM's singletics mode, no
# frame pacing) and, on completion, prints a line of the form:
#
#   Error: timed <gametics> gametics in <realtics> realtics
#
# where realtics are 1/35s units, so the average frame rate is
# gametics * 35 / realtics. ("Error:" is just how DOOM reports the result; it
# is not a failure.)
set -e

cd "$(dirname "$0")"

exec ./build_and_run.sh -timedemo demo1
