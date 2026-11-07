from decomp_path import DECOMP_DIR
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PYTHON = ROOT / 'local/venv/Scripts/python.exe'
EXE = Path(r'E:\Games\Fallout New Vegas\FalloutNV.exe')
DECOMP = Path(DECOMP_DIR)
TARGETS = ROOT / 'out/startup_lift_targets.json'
MAX_RUNS = 12


def known_code_starts():
    units = json.loads((DECOMP / 'build/units.json').read_text())['units']
    return {int(f['address'], 16) for u in units for f in u['functions']}


def main():
    known = known_code_starts()
    targets = {int(x, 16) for x in json.loads(TARGETS.read_text())} if TARGETS.exists() else {0x00B7A170}
    ROOT.joinpath('out').mkdir(exist_ok=True)
    for iteration in range(1, MAX_RUNS + 1):
        TARGETS.write_text(json.dumps([f'0x{x:08x}' for x in sorted(targets)], indent=1))
        result = subprocess.run(
            [str(PYTHON), str(ROOT / 'tools/cycle.py'), '--exe', str(EXE), '--skip-overlay', '--timeout', '300'],
            cwd=ROOT, capture_output=True, text=True, errors='replace')
        print(result.stdout[-1800:], flush=True)
        if result.returncode:
            print(result.stderr[-1500:], file=sys.stderr, flush=True)
            return result.returncode
        log = (ROOT / 'out/link/cycle_run.log').read_text(errors='replace')
        found = {int(x, 16) for x in re.findall(r'call into unbound original code at 0x([0-9a-fA-F]{8})', log)}
        new = (found & known) - targets
        summary = json.loads((ROOT / 'out/cycle_summary.json').read_text())
        if summary.get('exit') != 0 and not new:
            print(f'No new recipe targets at iteration {iteration}. Exit {summary.get("exit")}.')
            return 2
        if not new:
            print(f'No unresolved recipe call targets remain at iteration {iteration}.')
            return 0
        targets.update(new)
        print(f'Iteration {iteration}: queued {len(new)} newly observed recipe targets, total {len(targets)}.', flush=True)
    print(f'Reached {MAX_RUNS} runs with {len(targets)} queued targets. Resume from out/startup_lift_targets.json.')
    return 3


if __name__ == '__main__':
    sys.exit(main())

