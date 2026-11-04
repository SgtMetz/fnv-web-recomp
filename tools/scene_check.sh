#!/bin/bash
# Autonomous New Game check: headless run (FNV_BACKGROUND=1, transparent window) with an input script, then frames
# with brightness stats every N seconds after player placement, tagged with the last scripted input step.
# usage: tools/scene_check.sh [input script, default tools/input/newgame_dlc.txt] [frames, default 12] [interval s, 10]
#   -> out/scene/frame_NN.png and out/scene/report.txt; exits when the frames are taken (the game is killed)
cd "$(dirname "$0")/.." || exit 1
script="${1:-tools/input/newgame_dlc.txt}"; frames="${2:-12}"; every="${3:-10}"
mkdir -p out/scene; rm -f out/scene/frame_*.png
taskkill //F //IM FalloutNV_relinked.exe >/dev/null 2>&1
source tools/env.sh
export FNV_BACKGROUND=1 FNV_SKIP_INTRO=1 FNV_INPUT_FILE="$PWD/$script"
(tools/run_loader.sh 1800 > out/run_exit.txt 2>&1 &)
sleep 10
for i in $(seq 1 200); do
  grep -qE "INTRO Vsk|INWORLD|crash on thread" out/link/cycle_run.log && break
  tasklist | grep -qi relinked || break
  sleep 3
done
{
  echo "placement: $(grep -c 'INTRO Vsk' out/link/cycle_run.log) crash: $(grep -c 'crash on thread' out/link/cycle_run.log)"
  for n in $(seq -w 1 "$frames"); do
    sleep "$every"
    tasklist | grep -qi relinked || { echo "process gone before frame $n"; break; }
    step=$(grep -o "\[input\] step [0-9]* [a-z] [^ ]*.*" out/link/cycle_run.log | tail -1)
    stats=$(timeout 25 powershell -File tools/screenshot.ps1 -Out "$PWD/out/scene/frame_$n.png" -Stats | grep stats)
    echo "frame $n  ${stats:-capture failed}  last input: $step"
  done
  grep -n "crash on thread\|c0000374" out/link/cycle_run.log | head -3
} | tee out/scene/report.txt
taskkill //F //IM FalloutNV_relinked.exe >/dev/null 2>&1
