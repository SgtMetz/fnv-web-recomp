# Fallout: New Vegas in the browser

The original `FalloutNV.exe` is **statically recompiled**: every function is lifted to portable C++ and built for two
hosts, native Win32 and WebAssembly. A platform layer (PAL) stands in for Windows, Direct3D 9, DirectSound and
DirectShow.

The browser build plays New Game through character creation into Doc Mitchell's house:
- WebGPU rendering, including skinned meshes.
- Keyboard and mouse.
- Sound effects, music and voices.
- About 45-60 fps in the heavy interior and about 95 fps in menus.
- DLC is off by default.

See `docs/RECOMP.md` for the design.

## Credits

This project is built on **[fnv_decomp](https://github.com/momusdigital/fnv_decomp)**, the community decompilation
of the game ([fnvdecomp.com](https://fnvdecomp.com)). Its names and types are how every part of this port
understands the game. **[nv-rs](https://github.com/slaterain/nv-rs)** by slaterain supplied two design ideas,
and no code was copied from it. Fallout: New Vegas belongs to its owners: Obsidian Entertainment developed it and
Bethesda Softworks published it. This project is not affiliated with or endorsed by them, and it contains no game
files. The full list is in [CREDITS.md](CREDITS.md), and the site shows it at `/credits.html`.

**Licence:** [MIT](LICENSE), for this project's own code only. It grants nothing in the game or its files (see [NOTICE](NOTICE)).

## Folders

| Folder | What it is |
| --- | --- |
| `Claude fnv\` | This repository. |
| `Fallout New Vegas\` | The player's install. **Read-only: never modify it.** The tools read the exe and Data from here. |
| `fnv_decomp-main-updated\` | The community decompilation, used as the reference for names and understanding (`@fnv 0xADDR` annotations). Tools find it through `tools/decomp_path.py` (`FNV_DECOMP` overrides it). |
| `nv-rs-main\` | Another project (GPL-3.0). It was the source of two design ideas, and no code was copied. |


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

**Run:** start the server in development mode (`node server/serve.mjs --dev --game "E:/Games/Fallout New Vegas"`) and open `http://localhost:8093/`. `--dlc` turns
the DLC on.

On a touch screen (or with `?touch=1`) the page adds touch controls once the game is ready: a joystick on the left
for W/A/S/D, a drag elsewhere to look (a short tap is a click), and buttons for Fire, Aim, Use, Jump, Reload, Crouch,
Pip-Boy, VATS and Menu.

Before the game loads, the page asks how to start. `?start=normal` or `?start=quick` in the address skips the
question.
- **Normal:** the main menu, after about 60 s.
- **Quick:** a new game on Doc Mitchell's porch with every default taken, about 60 s after the choice. The opening is
  complete and "Back in the Saddle" is active. `port/recomp/pal_start.cpp` does this through the game's console
  path, and its header lists exactly what it sets.

**Debug in the page console:**
- `const d = await import('/debug.js')`, then:
  - `d.intoGame()` runs New Game into the world.
  - `d.keys([...DIK codes])`, `d.mouse(dx, dy)` and `d.click()` send input.
  - `d.snap('name')` saves a screenshot to `out/recomp/name.png`.
- `renderer.traceFrame()` captures a frame's draws.
- `Module.ccall('fnv_console', null, ['string'], ['GetStage 00104C1C'])` runs a console line on the main thread.
  - Name base forms and quests by form ID: only references keep their editor IDs at runtime.
  - The console's own text isn't drawn yet. Its output lines are a linked list at `ConsoleManager` (`*0x011d8ce8`)
    + 4, with nodes `{next, prev, char* text, u16 length}`. They are kept only while the console is open (the ~ key).
- `window.fnvPump` shows the command-ring pump counters.
- `window.fnvBoot` is the start-up progress behind the loading bar: download, compile, then the game's own loading
  (kilobytes its file layer fetched, `Module._fnv_boot()`, and the stage: main started, main menu, in the world, on
  the porch). The page also sends it as a `fnv-progress` event, and as a `{type: 'fnv-progress'}` postMessage to a
  page that embeds it in an iframe.
- `Module._fnv_dump_threads()` prints game-thread stacks.
- `Module._fnv_sound_debug()` lists the playing sound buffers.
- To find a function, map an address to the decomp with `grep -rn "@fnv 0x00XXXXXX" ../fnv_decomp-main-updated/src`,
  or read the lifted body in `out/recomp/recomp_NN.cpp` (`f_XXXXXXXX`).

**On a server:** `python -B tools/recomp_bundle.py` makes `out/dist/`, with the live page (no log or debug status; `?debug` in the address shows them), which `node server/serve.mjs --dist out/dist ...`
serves. HTTPS, a reverse proxy, systemd and the measured network behaviour are in `server/README.md`. `--delay 40`
(40 ms added to each file request) and `--dist out/dist` test both locally.

**Native host:** `bash tools/recomp_cycle.sh [seconds]` recompiles, builds `out/recomp/fnv_recomp.exe` and runs it
headless. The log goes to `out/recomp/run.log`.

## Repository map

| Path | Contents |
| --- | --- |
| `tools/recomp*.py`, `tools/gen_*.py` | **Active.** The recompiler, the builds (`recomp_bundle.py`: the deployment bundle), and the generated tables. |
| `server/` | **Active.** `serve.mjs`, the web server for both development and deployment, and `README.md`, the deployment guide. |
| `port/recomp/` | **Active.** Runtime and PAL: `fnv_recomp_rt.cpp` (CPU state, calls), `pal_kernel/user/fs/com/input.cpp`, `pal_d3d9.cpp` (writes the D3D9 command ring), `pal_dsound.cpp`, `pal_dshow.cpp`, and the hosts `host_win32.cpp` and `host_web.cpp`. |
| `web/recomp/` | **Active.** The browser page: `index.html` (worker start-up, ring pump, audio), `renderer.js` (WebGPU D3D9 renderer), `audio-worklet.js`, `debug.js`, and `replay.html` (plays a captured ring). |
| `tools/shader_*.py`, `tools/vkd3d_patches/`, `tools/build_vkd3d.py` | Shader translation from d3dbc to WGSL (`docs/SHADERS.md`). |
| `port/loader/`, `port/runtime/`, `port/compat/`, most other `tools/*.py` | **Dormant: Phases 1-2.** The native relink of the decomp C++ plus the lifter. It reached the Mojave natively, but it cannot target wasm because clang has no MSVC C++ ABI there. Kept for reference and for its analysis scans. |
| `web/` (outside `recomp/`), `web/README.md` | An earlier browser shell: install importer, saves, and an OPFS file layer. It is not wired to the recompiled build. |
| `spikes/` | Phase 0 experiments: a BSA viewer and a WGSL checker. |
| `out/` | Generated and ignored. `out/recomp/` (lifted sources, builds, `web_browser/`), `out/dist/` (the deployment bundle), `out/shaders`, `out/shader_manifest`, `out/fnv_docs` (private My Games copy), `out/link`, `out/obj`, `out/src`, `out/cache` (relink-era build products and caches). |
| `local/` | Ignored. Toolchains (`dl/emsdk`, `dl/vkd3d`, `dl/dawn`) and `venv/`. |

## Documents

- `docs/RECOMP.md`: the recompiler's design, milestones and performance notes.
- `docs/SHADERS.md`: the shader translation pipeline.
- `server/README.md`: serving the build, locally or on a server.
