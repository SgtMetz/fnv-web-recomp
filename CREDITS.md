# Credits

This project runs Fallout: New Vegas in the browser by recompiling the player's own copy of `FalloutNV.exe`. It
stands on other people's work. This page says whose work, and what we took from it.

## fnv_decomp: the foundation

**[fnv_decomp](https://github.com/momusdigital/fnv_decomp)**, the community decompilation coordinated on
[fnvdecomp.com](https://fnvdecomp.com), rebuilt the game's program as C++, function by function, with every
function checked to compile to the same bytes as the original.

What this project uses from it:
- **Names and understanding.** The recompiler, the platform layer and the debugging tools find their way around the
  game through fnv_decomp's names, types and layouts. The `@fnv 0xADDR` annotations in its sources map every
  address to a readable function.
- **The native relink (Phases 1 and 2, now dormant).** The first port compiled fnv_decomp's C++ directly and
  reached the Mojave natively. `port/compat/`, `port/loader/` and `port/runtime/` are the glue for that, not copies
  of fnv_decomp.

No fnv_decomp source is committed here. The tools read it from a local copy (`tools/decomp_path.py`).

fnv_decomp's authors chose to release it with no licence, and they have said that credit is not required. We
credit them anyway, because without them this project would not exist. Their contributors chose to be named in
[fnv_decomp's CREDITS.md](https://github.com/momusdigital/fnv_decomp/blob/main/CREDITS.md). The largest
contributions came from connorx2000, h-x3n, L1ll3J, andgju, gavinbrow, FFwLo, MK5280, momentom861, MatiasIsGood and
Stargallian, with 35 more named contributors and 8 who took part anonymously.

## nv-rs: two design ideas

**[nv-rs](https://github.com/slaterain/nv-rs)**, by slaterain and its contributors, is a new engine for Fallout:
New Vegas written in Rust (GPL-3.0-or-later). We studied it and took two ideas from it:
- running the game's threads as deterministic coroutines;
- modelling Direct3D 9 as a complete state machine, separate from any real graphics API.

No nv-rs code was copied, so its licence does not apply to this project. The evaluation is in
`docs/NVRS_EVALUATION.md`.

## Tools and libraries

- [LLVM/Clang](https://llvm.org) and [Emscripten](https://emscripten.org) build the recompiled game for the browser.
- [Capstone](https://www.capstone-engine.org) disassembles the exe for the recompiler.
- [vkd3d](https://gitlab.winehq.org/wine/vkd3d) and [Tint](https://dawn.googlesource.com/dawn) (from Dawn)
  translate the game's Direct3D 9 shaders to WGSL.
- [Cloudflare Workers and R2](https://developers.cloudflare.com/workers/) host the private deployment.

## The game

Fallout: New Vegas was developed by Obsidian Entertainment and published by Bethesda Softworks. "Fallout",
"Fallout: New Vegas" and "Bethesda" are trademarks of their owners. This project is not affiliated with or endorsed
by Bethesda Softworks, ZeniMax Media, Obsidian Entertainment or Microsoft.

This repository contains no game files, no executables and nothing derived from them. To use it you need your own
copy of the game.

## Licence

This project's own code is under the [MIT licence](LICENSE): anyone may use, copy, change and share it, provided
the licence notice is kept. The licence covers only our original work, not the game, its files, anything generated
from them, or fnv_decomp's material.
