#!/bin/bash
# usage: tools/run_loader.sh [timeout seconds]  -> out/link/cycle_run.log (same file the cycle writes)
cd "$(dirname "$0")/../out/link" || exit 1
timeout "${1:-90}" ./FalloutNV_relinked.exe --exe "E:/Games/Fallout New Vegas/FalloutNV.exe" > cycle_run.log 2>&1
echo "exit $?"
