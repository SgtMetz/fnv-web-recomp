# Shader translation (d3dbc -> SPIR-V -> WGSL)

**Author:** Claude Sonnet 5.5, 2026-11-03. Read this before touching the renderer's shader path or the vkd3d patches.

Status: done, committed as `e944f2f` in `E:\Games\Claude fnv`. 15,791 / 15,791 shaders translate (all 16 `.sdp`
packages, 100% each; package 013 = 1,007/1,007). Baseline was 96.8%. No failures remain.
Short version is also in `PROGRESS.md`.

## Pipeline
`.sdp` (in `E:\Games\Fallout New Vegas\Data\Shaders\`) -> `tools/shader_spike.py` unpacks to `out/shaders/<pkg>/*.dxbc`
(raw d3dbc token streams) -> `vkd3d-compiler -x d3dbc -b spirv-binary` -> `tint -f wgsl --allow-non-uniform-derivatives true`
-> `*.wgsl.gen` next to each shader. Everything under `out/` and `local/` is git-ignored (rule: nothing derived from game
data is committed).

## Tools (all committed)
- `python tools/build_vkd3d.py [--reset]` - patches + rebuilds vkd3d-shader 1.19 with zig cc into
  `local/dl/vkd3d/build/vkd3d-compiler.exe`. `--reset` re-extracts the pristine tarball and applies
  `tools/vkd3d_patches/*.patch` (needed after adding a patch; without it the existing source tree is rebuilt as is).
  HLSL/preproc front end is stubbed (`local/dl/vkd3d/build/hlsl_stubs.c`): d3dbc input does not need it, HLSL compile does.
- `python tools/shader_spike.py <pkg.sdp | out/shaders/<pkg>> ...` - runs the pipeline, prints pass rates and bucketed
  reasons, writes `out/shader_results/<pkg>.jsonl` (sha1 of token stream, name, profile, stage, reason) and
  `out/shader_spike.json`. Duplicate shader names inside a package get a `#N` suffix in the file name.
- `python tools/shader_validate.py [dirs] [--limit N] [--names a,b]` - compares each original's disassembly
  (`vkd3d-compiler -b d3d-asm`) with its WGSL: input regs == entry params, samplers == t/s bindings, highest non-`def`
  constant read fits the cb array, written outputs == WGSL outputs. Currently 0 differences (013, 002, 009 = 1,837 shaders
  at last full run; 013 alone 1,007).

## vkd3d patch (`tools/vkd3d_patches/0001-d3dbc-translation-fixes.patch`, touches ir.c, spirv.c, d3dbc.c)
1. `vsir_program_lower_lit` (ir.c, d3dbc lowering pass): D3D9 lit. x=1, y=max(x,0), z = x>0 ? pow(max(y,0), clamp(w,-127.99,127.99)) : 0,
   w=1. pow expanded like vkd3d's own pow lowering so 0^0 = 1.
2. `vsir_program_lower_breakc`: `breakc_<cmp>` -> compare, `if`, `break`, `endif`. (Plain SM3 `break` already worked;
   the Phase 0 label was misleading.)
3. `spirv_widen_signature_mask` (spirv.c, emit_input/emit_output): signature masks not starting at x are widened to start
   at x, so vkd3d never emits the SPIR-V `Component` decoration that Tint rejects. Each register keeps its own location.
4. MISCTYPE (ps_3_0 vPos idx 0 / vFace idx 1) sources are bound to the SV_POSITION / SV_IS_FRONT_FACE input signature
   elements in the io normaliser (ir.c); `oDepth` sets `io_dcls[DEPTHOUT]` in d3dbc.c so spirv emits it via its dedicated
   register path. Only SKYSUNOCCL and VolumetricFog001 (x16 packages) hit this.
5. Tint: `--allow-non-uniform-derivatives true` (in shader_spike.py) instead of per-shader fixes.

Gotchas: `git apply` inside `local/` silently applies nothing (the outer repo ignores it); `build_vkd3d.py` applies from the
repo root with `--directory`. `.gitattributes` has `*.patch -text` so patches keep LF.

## Interface manifest
`python tools/shader_manifest.py [out/shaders/<pkg> ...]` writes `out/shader_manifest/<pkg>.json` (git-ignored), one entry per
shader: stage, profile, sha1, wgsl file name, `inputs` / `outputs` (name, `location` or `builtin`, type, interpolation;
vertex inputs also carry the D3D semantic from the `dcl`, e.g. `position0`, `texcoord0`), and `bindings` (samplers with
texture/sampler `@binding` numbers and dim `2d`/`cube`, and the constant array's binding and vec4 count). Built for the D3D9
shim so it can create bind group / pipeline layouts and match vertex outputs to pixel inputs (same `location` = same D3D
register: oT4 -> location 4, colors 8+) without parsing WGSL. All 15,791 entries have outputs and complete sampler pairs.

## Facts about the generated WGSL (relevant for the renderer)
- Constants: one uniform array per shader, `cb0_0.tint_symbol[N]` (vec4), `@group(0) @binding(K)`, K after the texture/
  sampler pairs. Array length = highest non-`def` constant index read + 1. `def` constants are inlined; `defi`/`defb` only
  loosely checked. Relative addressing (a0, aL) is not specially validated.
- Textures/samplers: `tN` (texture_2d etc.) and `sN` (sampler) as separate bindings, interleaved `t0,s0,t1,s1,...`.
- Registers are `var<private> rN`; entry `main` takes `@location` params. Vertex shaders emit extra outputs: vkd3d adds
  the D3D9 default diffuse (1,1,1,1) at location 8 when the VS does not write oD0; outputs are a struct `tint_symbol_1`.
  Locations come from D3D register numbers (colors offset 8; interpolators keep their register index).
- vFace arrives as `@builtin(front_facing)` bool, converted to +/-1 float in the body.
- Package choice: the port should report NVIDIA SM3 + FP16 caps so the game selects package 013 (see history/PHASE0_REPORT.md).

## Browser compile check (done)
`spikes/wgsl_check/` (launch config `wgsl-check`, port 8660) fetches every `out/shaders/*/*.wgsl.gen` from a local-only
server and calls `createShaderModule` + `getCompilationInfo`. Result in the built-in browser (Chromium WebGPU, AMD
adapter): 15,791 / 15,791 compile, 0 errors, 0 warnings. `?pkg=shaderpackage013` limits it to one package;
results are in `window.__result`. Tint also re-parses all of package 013 as WGSL input.

## Not verified / next steps
- Nothing has been rendered with these shaders yet (compiling is not the same as drawing correctly).
- A numeric check of `lit` against a reference (read only against the D3D9 formula so far).
- Cache plan: translated shaders go in OPFS under `fnv/cache/` (web track); translation itself is offline (Python/zig +
  tint here), so the browser would ship pre-translated WGSL generated from the user's own files at import time or on first run.
  Decide whether that happens in-browser (needs vkd3d + tint compiled to wasm) or via a user-side tool.
