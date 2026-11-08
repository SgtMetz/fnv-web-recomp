# Fallout: New Vegas in the browser

Start here. This page is the map: what works, how to build and run it, where the code is, and which documents are
current. The detailed history lives in `PROGRESS.md`, which is append-only, with the newest entry at the end.

## Where it stands (2026-11-08)

The original `FalloutNV.exe` is **statically recompiled**: every function is lifted to portable C++ and built for two
hosts, native Win32 and WebAssembly. A platform layer (PAL) stands in for Windows, Direct3D 9, DirectSound and
DirectShow.

The browser build plays New Game through character creation into Doc Mitchell's house:
- WebGPU rendering, including skinned meshes.
- Keyboard and mouse.
- Sound effects, music and voices.
- About 45-60 fps in the heavy interior and about 95 fps in menus.
- DLC is off by default.

See `docs/RECOMP.md` for the design and `PROGRESS.md` for the evidence.

## Folders on this machine (`E:\Games`)

| Folder | What it is |
| --- | --- |
| `Claude fnv\` | This repository. |
| `Fallout New Vegas\` | The player's install. **Read-only: never modify it.** The tools read the exe and Data from here. |
| `fnv_decomp-main-updated\` | The community decompilation, used as the reference for names and understanding (`@fnv 0xADDR` annotations). Tools find it through `tools/decomp_path.py` (`FNV_DECOMP` overrides it). |
| `nv-rs-main\` | Another project (GPL-3.0). It was the source of two design ideas, and no code was copied. See `docs/PHASE2_PLAN.md` and `docs/NVRS_EVALUATION.md`. |

## Rules for anyone working here

- **Never commit anything derived from the exe or the game data.** `out/` (generated) and `local/` (toolchains,
  venv) are git-ignored for that reason.
- **Never touch the real `Documents\My Games\FalloutNV`.** The builds use a private copy in `out/fnv_docs`, which the
  web host serves as `/docs`.
- **The dev server binds to 127.0.0.1 only.** Game files never leave the machine.
- **Git:**
  - Stage explicit paths, never `git add -A`.
  - Don't switch branches. Other sessions may share this checkout.
  - Commit as `git -c user.name=SgtMetz -c user.email=69877075+SgtMetz@users.noreply.github.com commit`.
- **Record results in `PROGRESS.md`:** add a dated entry with what was measured, what was fixed, and the evidence.

## Build and run the browser version

The prerequisites are already installed:
- LLVM in `C:\Program Files\LLVM`.
- Python with `capstone`.
- emsdk 4.0.10 in `local/dl/emsdk`.
- vkd3d and tint (dawn) in `local/dl`, for shaders.

```bash
EXE="E:/Games/Fallout New Vegas/FalloutNV.exe"
python -B tools/recomp.py --exe "$EXE" --shards 64   # lift the exe -> out/recomp/recomp_NN.cpp (~95 s)
python -B tools/gen_import_table.py --exe "$EXE"     # out/recomp/import_table.inc
python -B tools/gen_com_tables.py                    # out/recomp/com_tables.inc
python -B tools/recomp_build_web.py --target browser # -O2, ~3 min cold -> out/recomp/web_browser/
```

`out/recomp/d3d9_caps.inc` comes from `tools/d3d9_caps_dump.cpp`; the build line is in that file. The translated
shaders (`out/shaders`, `out/shader_manifest`) come from the pipeline in `docs/SHADERS.md`. All of these already
exist, so you only rerun a step when its inputs change.

**Run:** start the dev server (launch config `recomp` in `.claude/launch.json`, or
`node tools/recomp_serve.mjs --game "E:/Games/Fallout New Vegas"`) and open `http://localhost:8093/`. The main menu
appears after about 60 s. `--dlc` turns the DLC on.

**Debug in the page console:**
- `const d = await import('/debug.js')`, then:
  - `d.intoGame()` runs New Game into the world.
  - `d.keys([...DIK codes])`, `d.mouse(dx, dy)` and `d.click()` send input.
  - `d.snap('name')` saves a screenshot to `out/recomp/name.png`.
- `renderer.traceFrame()` captures a frame's draws.
- `window.fnvPump` shows the command-ring pump counters.
- `Module._fnv_dump_threads()` prints game-thread stacks.
- `Module._fnv_sound_debug()` lists the playing sound buffers.
- To find a function, map an address to the decomp with `grep -rn "@fnv 0x00XXXXXX" ../fnv_decomp-main-updated/src`,
  or read the lifted body in `out/recomp/recomp_NN.cpp` (`f_XXXXXXXX`).

**Native host:** `bash tools/recomp_cycle.sh [seconds]` recompiles, builds `out/recomp/fnv_recomp.exe` and runs it
headless. The log goes to `out/recomp/run.log`.

## Repository map

| Path | Contents |
| --- | --- |
| `tools/recomp*.py`, `tools/recomp_serve.mjs`, `tools/gen_*.py` | **Active.** The recompiler, the builds, the browser dev server, and the generated tables. |
| `port/recomp/` | **Active.** Runtime and PAL: `fnv_recomp_rt.cpp` (CPU state, calls), `pal_kernel/user/fs/com/input.cpp`, `pal_d3d9.cpp` (writes the D3D9 command ring), `pal_dsound.cpp`, `pal_dshow.cpp`, and the hosts `host_win32.cpp` and `host_web.cpp`. |
| `web/recomp/` | **Active.** The browser page: `index.html` (worker start-up, ring pump, audio), `renderer.js` (WebGPU D3D9 renderer), `audio-worklet.js`, `debug.js`, and `replay.html` (plays a captured ring). |
| `tools/shader_*.py`, `tools/vkd3d_patches/`, `tools/build_vkd3d.py` | Shader translation from d3dbc to WGSL (`docs/SHADERS.md`). |
| `port/loader/`, `port/runtime/`, `port/compat/`, most other `tools/*.py` | **Dormant: Phases 1-2.** The native relink of the decomp C++ plus the lifter. It reached the Mojave natively, but it cannot target wasm because clang has no MSVC C++ ABI there. Kept for reference and for its analysis scans. |
| `web/` (outside `recomp/`), `web/README.md` | An earlier browser shell: install importer, saves, and an OPFS file layer. It is not wired to the recompiled build. |
| `spikes/` | Phase 0 experiments: a BSA viewer and a WGSL checker. |
| `out/` | Generated and ignored. `out/recomp/` (lifted sources, builds, `web_browser/`), `out/shaders`, `out/shader_manifest`, `out/fnv_docs` (private My Games copy), `out/link`, `out/obj`, `out/src`, `out/cache` (relink-era build products and caches). |
| `local/` | Ignored. Toolchains (`dl/emsdk`, `dl/vkd3d`, `dl/dawn`) and `venv/`. |

## Documents

Current:
- `docs/RECOMP.md`: the recompiler's design, milestones and performance notes.
- `docs/SHADERS.md`: the shader translation pipeline.
- `docs/DECOMP_UPDATE.md`: how the decomp update was handled, and its checklist.
- `PROGRESS.md`: the full dated log. Read the last few entries before starting.

Historical, kept for context: `docs/PHASE2_PLAN.md`, `docs/NVRS_EVALUATION.md`, and `docs/history/` (the Phase 0
report, the original roadmap, and the 2026-11-03 handoff). These describe the relink path that preceded the
recompiler.

## Open work

- **Performance:** the remaining cost per draw is the WebGPU calls themselves, so the next steps are fewer bind-group
  switches and render bundles. A flag-liveness pass in the recompiler would speed up the game thread.
- **Gameplay coverage beyond Doc Mitchell's house:** leaving Goodsprings, saving and loading, the Pip-Boy, combat.
- **The native host still loads the DLC:** its file access goes straight to the install.
