#!/bin/bash
# Recompile, build and run the native whole-game build (docs/RECOMP.md), headless (FNV_BACKGROUND=1).
# usage: tools/recomp_cycle.sh [seconds, default 90] [--no-recomp]   -> out/recomp/run.log
cd "$(dirname "$0")/.." || exit 1
secs="${1:-90}"
EXE="E:/Games/Fallout New Vegas/FalloutNV.exe"
if [ "$2" != "--no-recomp" ]; then
  python -B tools/recomp.py --exe "$EXE" --shards 64 > out/recomp_run.log 2>&1 || { tail -20 out/recomp_run.log; exit 1; }
  python -c "import json;r=json.load(open('out/recomp/report.json'));print('recomp: %d functions, %d unsupported, %ds' % (r['lifted'], r['unsupported'], r['seconds']))"
fi
python -B tools/recomp_build.py 2>&1 | tail -1 || exit 1
cd out/recomp && FNV_BACKGROUND=1 timeout "$secs" ./fnv_recomp.exe --exe "$EXE"
echo "exit $?"
grep -v "could not reserve" run.log | head -40
