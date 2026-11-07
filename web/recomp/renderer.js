// WebGPU renderer for the whole-game recompiler (docs/RECOMP.md, milestone 3). It consumes the Direct3D 9 command
// stream that port/recomp/pal_d3d9.cpp writes (live from the wasm heap, or a captured ring.bin) and draws it with the
// game's own shaders, translated ahead of time to WGSL (out/shaders, served by tools/recomp_serve.mjs at /shader/<sha1>).
//
// D3D9 -> WebGPU in brief: render states become pipeline state (cached by key), constants go to per-submit uniform
// arenas with dynamic offsets, UP draws to a per-submit vertex arena, and an upload to a resource already used by
// unsubmitted commands first submits them (D3D9 lets the game rewrite a buffer between draws of one frame).

export const OP = {
  WRAP: 0xffff, CREATE_TEXTURE: 1, DESTROY: 2, UPLOAD_TEXTURE: 3, CREATE_BUFFER: 4, UPLOAD_BUFFER: 5, CREATE_SHADER: 6,
  CREATE_DECL: 7, RENDER_STATE: 8, SAMPLER_STATE: 9, SET_TEXTURE: 10, SET_SHADER: 11, SHADER_CONST: 12, SET_STREAM: 13,
  SET_INDICES: 14, SET_DECL: 15, SET_FVF: 16, SET_TARGET: 17, SET_DEPTH: 18, VIEWPORT: 19, SCISSOR: 20, CLEAR: 21,
  DRAW: 22, DRAW_INDEXED: 23, DRAW_UP: 24, DRAW_INDEXED_UP: 25, STRETCH: 26, PRESENT: 27, STREAM_FREQ: 28,
  COLOR_FILL: 29, TEX_STAGE_STATE: 30, RESIZE: 31, SET_TRANSFORM: 32,
};

const fourcc = (s) => s.charCodeAt(0) | (s.charCodeAt(1) << 8) | (s.charCodeAt(2) << 16) | (s.charCodeAt(3) << 24);
const FMT = { DXT1: fourcc('DXT1'), DXT2: fourcc('DXT2'), DXT3: fourcc('DXT3'), DXT4: fourcc('DXT4'), DXT5: fourcc('DXT5'),
  INTZ: fourcc('INTZ'), NULL: fourcc('NULL'), ATI1: fourcc('ATI1'), ATI2: fourcc('ATI2') };
const DEPTH_FORMATS = new Set([71, 75, 77, 79, 80, 82, 83, FMT.INTZ]);

// D3DFORMAT -> {gpu format, conversion of uploaded data, block size}
function formatInfo(fmt) {
  switch (fmt >>> 0) {
    case 21: return { gpu: 'bgra8unorm', bpp: 4 };
    case 22: return { gpu: 'bgra8unorm', bpp: 4, conv: 'xrgb' };
    case 32: return { gpu: 'rgba8unorm', bpp: 4 };
    case 33: return { gpu: 'rgba8unorm', bpp: 4, conv: 'xrgb' };
    case 31: return { gpu: 'rgb10a2unorm', bpp: 4 };
    case 23: return { gpu: 'bgra8unorm', bpp: 2, conv: 'r5g6b5' };
    case 24: return { gpu: 'bgra8unorm', bpp: 2, conv: 'x1r5g5b5' };
    case 25: return { gpu: 'bgra8unorm', bpp: 2, conv: 'a1r5g5b5' };
    case 26: return { gpu: 'bgra8unorm', bpp: 2, conv: 'a4r4g4b4' };
    case 28: return { gpu: 'bgra8unorm', bpp: 1, conv: 'a8' };
    case 50: return { gpu: 'bgra8unorm', bpp: 1, conv: 'l8' };
    case 51: return { gpu: 'bgra8unorm', bpp: 2, conv: 'a8l8' };
    case 111: return { gpu: 'r16float', bpp: 2 };
    case 112: return { gpu: 'rg16float', bpp: 4 };
    case 113: return { gpu: 'rgba16float', bpp: 8 };
    case 114: return { gpu: 'r32float', bpp: 4, unfilterable: true };
    case 115: return { gpu: 'rg32float', bpp: 8, unfilterable: true };
    case 116: return { gpu: 'rgba32float', bpp: 16, unfilterable: true };
    case FMT.DXT1: return { gpu: 'bc1-rgba-unorm', block: 8 };
    case FMT.DXT2: case FMT.DXT3: return { gpu: 'bc2-rgba-unorm', block: 16 };
    case FMT.DXT4: case FMT.DXT5: return { gpu: 'bc3-rgba-unorm', block: 16 };
    case FMT.ATI1: return { gpu: 'bc4-r-unorm', block: 8 };
    case FMT.ATI2: return { gpu: 'bc5-rg-unorm', block: 16 };
    default:
      if (DEPTH_FORMATS.has(fmt >>> 0)) return { gpu: 'depth24plus-stencil8', depth: true, unfilterable: true };
      return { gpu: 'bgra8unorm', bpp: 4, unknown: true };
  }
}

// expand 8/16-bit formats to BGRA8
function convertPixels(conv, src, w, h, srcRow) {
  const out = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const o = (y * w + x) * 4;
      let b, g, r, a;
      if (conv === 'xrgb') { const s = y * srcRow + x * 4; b = src[s]; g = src[s + 1]; r = src[s + 2]; a = 255; }
      else if (conv === 'a8') { b = g = r = 0; a = src[y * srcRow + x]; }
      else if (conv === 'l8') { b = g = r = src[y * srcRow + x]; a = 255; }
      else if (conv === 'a8l8') { const s = y * srcRow + x * 2; b = g = r = src[s]; a = src[s + 1]; }
      else {
        const s = y * srcRow + x * 2, v = src[s] | (src[s + 1] << 8);
        if (conv === 'r5g6b5') { r = ((v >> 11) & 31) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31; a = 255; }
        else if (conv === 'a4r4g4b4') { a = ((v >> 12) & 15) * 17; r = ((v >> 8) & 15) * 17; g = ((v >> 4) & 15) * 17; b = (v & 15) * 17; }
        else { r = ((v >> 10) & 31) * 255 / 31; g = ((v >> 5) & 31) * 255 / 31; b = (v & 31) * 255 / 31; a = conv === 'a1r5g5b5' ? (v & 0x8000 ? 255 : 0) : 255; }
      }
      out[o] = b; out[o + 1] = g; out[o + 2] = r; out[o + 3] = a;
    }
  }
  return out;
}

// D3DDECLTYPE -> vertex format, byte size, shader base type
const DECL_TYPES = [
  ['float32', 4, 'f32'], ['float32x2', 8, 'f32'], ['float32x3', 12, 'f32'], ['float32x4', 16, 'f32'],
  ['unorm8x4-bgra', 4, 'f32'], ['uint8x4', 4, 'u32'], ['sint16x2', 4, 'i32'], ['sint16x4', 8, 'i32'],
  ['unorm8x4', 4, 'f32'], ['snorm16x2', 4, 'f32'], ['snorm16x4', 8, 'f32'], ['unorm16x2', 4, 'f32'],
  ['unorm16x4', 8, 'f32'], ['unorm10-10-10-2', 4, 'f32'], ['unorm10-10-10-2', 4, 'f32'], ['float16x2', 4, 'f32'],
  ['float16x4', 8, 'f32'],
];
const USAGE_NAMES = ['position', 'blendweight', 'blendindices', 'normal', 'psize', 'texcoord', 'tangent', 'binormal',
  'tessfactor', 'positiont', 'color', 'fog', 'depth', 'sample'];
// the semantic strings a declaration element can satisfy (the shader manifests' spelling)
function semanticsOf(usage, index) {
  const n = USAGE_NAMES[usage] || 'unknown';
  const s = [n + index];
  if (index === 0) s.push(n);                       // the manifests drop the 0 on tangent, binormal and color
  if (usage === 1) s.push('weight');
  if (usage === 2) s.push('blend');
  if (usage === 10 && index === 1) s.push('specular0');
  if (usage === 9) s.push('position' + index);
  return s;
}

// FVF -> declaration elements (stream 0)
function fvfElements(fvf) {
  const el = [];
  let off = 0;
  const pos = fvf & 0x400e;
  if (pos === 0x004) { el.push({ stream: 0, offset: off, type: 3, usage: 9, index: 0 }); off += 16; }
  else if (pos) {
    el.push({ stream: 0, offset: off, type: pos === 0x4002 ? 3 : 2, usage: 0, index: 0 });
    off += pos === 0x4002 ? 16 : 12;
    const betas = pos >= 0x006 && pos <= 0x00e ? (pos - 0x004) / 2 : 0;
    if (betas) { el.push({ stream: 0, offset: off, type: Math.min(betas, 4) - 1, usage: 1, index: 0 }); off += 4 * betas; }
  }
  if (fvf & 0x010) { el.push({ stream: 0, offset: off, type: 2, usage: 3, index: 0 }); off += 12; }
  if (fvf & 0x020) { el.push({ stream: 0, offset: off, type: 0, usage: 4, index: 0 }); off += 4; }
  if (fvf & 0x040) { el.push({ stream: 0, offset: off, type: 4, usage: 10, index: 0 }); off += 4; }
  if (fvf & 0x080) { el.push({ stream: 0, offset: off, type: 4, usage: 10, index: 1 }); off += 4; }
  const ntex = (fvf >> 8) & 15;
  for (let i = 0; i < ntex; i++) {
    const f = (fvf >> (16 + 2 * i)) & 3;
    const comps = [2, 3, 4, 1][f];
    el.push({ stream: 0, offset: off, type: comps - 1, usage: 5, index: i });
    off += 4 * comps;
  }
  return el;
}

const BLEND = [null, 'zero', 'one', 'src', 'one-minus-src', 'src-alpha', 'one-minus-src-alpha', 'dst-alpha',
  'one-minus-dst-alpha', 'dst', 'one-minus-dst', 'src-alpha-saturated', 'src-alpha', 'one-minus-src-alpha', 'constant',
  'one-minus-constant'];
const BLEND_ALPHA = (f) => ({ src: 'src-alpha', 'one-minus-src': 'one-minus-src-alpha', dst: 'dst-alpha', 'one-minus-dst': 'one-minus-dst-alpha' }[f] || f);
const BLENDOP = [null, 'add', 'subtract', 'reverse-subtract', 'min', 'max'];
const CMP = [null, 'never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'];
const STENCILOP = [null, 'keep', 'zero', 'replace', 'increment-clamp', 'decrement-clamp', 'invert', 'increment-wrap', 'decrement-wrap'];
const ADDRESS = [null, 'repeat', 'mirror-repeat', 'clamp-to-edge', 'clamp-to-edge', 'clamp-to-edge'];
const RS = { ZENABLE: 7, FILLMODE: 8, ZWRITEENABLE: 14, ALPHATESTENABLE: 15, SRCBLEND: 19, DESTBLEND: 20, CULLMODE: 22,
  ZFUNC: 23, ALPHAREF: 24, ALPHAFUNC: 25, ALPHABLENDENABLE: 27, STENCILENABLE: 52, STENCILFAIL: 53, STENCILZFAIL: 54,
  STENCILPASS: 55, STENCILFUNC: 56, STENCILREF: 57, STENCILMASK: 58, STENCILWRITEMASK: 59, COLORWRITEENABLE: 168,
  BLENDOP: 171, SCISSORTESTENABLE: 174, SLOPESCALEDEPTHBIAS: 175, TWOSIDEDSTENCILMODE: 185, CCW_STENCILFAIL: 186,
  CCW_STENCILZFAIL: 187, CCW_STENCILPASS: 188, CCW_STENCILFUNC: 189, COLORWRITEENABLE1: 190, BLENDFACTOR: 193,
  DEPTHBIAS: 195, SEPARATEALPHABLENDENABLE: 206, SRCBLENDALPHA: 207, DESTBLENDALPHA: 208, BLENDOPALPHA: 209 };

function defaultRenderStates() {
  const rs = new Uint32Array(256);
  rs[RS.ZENABLE] = 1; rs[RS.FILLMODE] = 3; rs[RS.ZWRITEENABLE] = 1; rs[RS.SRCBLEND] = 2; rs[RS.DESTBLEND] = 1;
  rs[RS.CULLMODE] = 3; rs[RS.ZFUNC] = 4; rs[RS.ALPHAFUNC] = 8; rs[RS.STENCILFAIL] = 1; rs[RS.STENCILZFAIL] = 1;
  rs[RS.STENCILPASS] = 1; rs[RS.STENCILFUNC] = 8; rs[RS.STENCILMASK] = 0xffffffff; rs[RS.STENCILWRITEMASK] = 0xffffffff;
  rs[RS.COLORWRITEENABLE] = 15; rs[RS.BLENDOP] = 1; rs[RS.CCW_STENCILFAIL] = 1; rs[RS.CCW_STENCILZFAIL] = 1;
  rs[RS.CCW_STENCILPASS] = 1; rs[RS.CCW_STENCILFUNC] = 8; rs[RS.COLORWRITEENABLE1] = 15; rs[191] = 15; rs[192] = 15;
  rs[RS.BLENDFACTOR] = 0xffffffff; rs[RS.SRCBLENDALPHA] = 2; rs[RS.DESTBLENDALPHA] = 1; rs[RS.BLENDOPALPHA] = 1;
  return rs;
}
function defaultSamplerStates() {
  const ss = [];
  for (let i = 0; i < 20; i++) ss.push(Uint32Array.from([0, 1, 1, 1, 0, 2, 2, 0, 0, 0, 1, 0, 0, 0]));
  return ss;
}

// ---- WGSL entry-point rewriting ---------------------------------------------------------------------------------------
// The translated shaders keep D3D's own linkage; WebGPU needs exact vertex/fragment interface matches and has no
// alpha test, so the entry point is rewritten per pipeline: parameters are retyped and the original names rebuilt
// with `let` in a prelude.
function splitEntry(wgsl, stage) {
  const at = wgsl.indexOf('@' + stage);
  const fn = wgsl.indexOf('fn main(', at);
  let i = fn + 8, depth = 1;
  while (depth) { const c = wgsl[i++]; if (c === '(') depth++; else if (c === ')') depth--; }
  const paramText = wgsl.slice(fn + 8, i - 1);
  const brace = wgsl.indexOf('{', i);
  const params = [];
  let cur = '', d = 0;
  for (const c of paramText) {
    if (c === '(' || c === '<') d++;
    if (c === ')' || c === '>') d--;
    if (c === ',' && d === 0) { params.push(cur); cur = ''; } else cur += c;
  }
  if (cur.trim()) params.push(cur);
  const parsed = params.map((p) => {
    const m = /^([\s\S]*?)([A-Za-z_]\w*)\s*:\s*([\s\S]+)$/.exec(p.trim());
    const attrs = m[1].trim();
    const loc = /@location\((\d+)u?\)/.exec(attrs);
    return { attrs, name: m[2], type: m[3].trim(), location: loc ? +loc[1] : null };
  });
  return { head: wgsl.slice(0, fn), params: parsed, tail: wgsl.slice(i - 1, brace + 1), body: wgsl.slice(brace + 1) };
}
function joinEntry(e, params, prelude) {
  return e.head + 'fn main(' + params.join(', ') + e.tail + '\n' + prelude.join('\n') + e.body;
}
const COMPS = { 'f32': 1, 'vec2<f32>': 2, 'vec3<f32>': 3, 'vec4<f32>': 4 };
function convertExpr(x, from, to) {
  const a = COMPS[from], b = COMPS[to];
  if (!a || !b || a === b) return x;
  if (b < a) return x + '.' + 'xyzw'.slice(0, b);
  const fill = ['0.0', '0.0', '0.0', '1.0'].slice(a, b);
  return `${to}(${x}, ${fill.join(', ')})`;
}

export class Renderer {
  constructor(device, canvas, opts = {}) {
    this.dev = device;
    this.canvas = canvas;
    this.ctx = canvas.getContext('webgpu');
    this.canvasFormat = navigator.gpu.getPreferredCanvasFormat();
    this.ctx.configure({ device, format: this.canvasFormat, alphaMode: 'opaque' });
    this.fetchShader = opts.fetchShader || ((sha) => fetch('/shader/' + sha).then((r) => (r.ok ? r.json() : null)));
    this.log = opts.log || ((m) => console.log('[renderer] ' + m));
    this.warned = new Set();
    this.textures = new Map();
    this.buffers = new Map();
    this.shaders = new Map();
    this.decls = new Map();
    this.modules = new Map();
    this.pipelines = new Map();
    this.layouts = new Map();
    this.samplers = new Map();
    this.bindGroups = new Map();
    this.rs = defaultRenderStates();
    this.ss = defaultSamplerStates();
    this.tex = new Array(20).fill(0);
    this.vs = 0; this.ps = 0;
    this.vsConst = new Float32Array(256 * 4);
    this.psConst = new Float32Array(224 * 4);
    this.streams = [];
    for (let i = 0; i < 16; i++) this.streams.push({ id: 0, offset: 0, stride: 0, freq: 1 });
    this.indices = 0; this.decl = 0; this.fvf = 0;
    this.rt = [{ id: 1, face: 0, level: 0 }, null, null, null];
    this.depth = 0;
    this.viewport = [0, 0, 1280, 720, 0, 1];
    this.scissor = [0, 0, 1280, 720];
    this.frames = 0;
    this.stats = { draws: 0, skipped: 0, submits: 0 };
    this.used = new Set();
    this.uniformArena = device.createBuffer({ size: 16 << 20, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    this.uniformStage = new Uint8Array(16 << 20);
    this.uniformUsed = 0;
    this.vertexArena = device.createBuffer({ size: 32 << 20, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
    this.vertexStage = new Uint8Array(32 << 20);
    this.vertexUsed = 0;
    this.zeroVB = device.createBuffer({ size: 64, usage: GPUBufferUsage.VERTEX });
    this.encoder = device.createCommandEncoder();
    this.pass = null;
    this.resize(1280, 720);
    this.makeDummies();
    this.makeBlit();
    device.addEventListener?.('uncapturederror', (e) => this.warn('gpu: ' + e.error.message.slice(0, 400)));
  }

  warn(m) { if (!this.warned.has(m)) { this.warned.add(m); this.log(m); } }

  makeDummies() {
    const mk = (dim, layers) => {
      const t = this.dev.createTexture({ size: [1, 1, layers], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST, dimension: dim === '3d' ? '3d' : '2d' });
      for (let l = 0; l < layers; l++) this.dev.queue.writeTexture({ texture: t, origin: [0, 0, l] }, new Uint8Array([0, 0, 0, 255]), { bytesPerRow: 4 }, [1, 1, 1]);
      return t.createView({ dimension: dim });
    };
    this.dummy = { '2d': mk('2d', 1), cube: mk('cube', 6), '3d': mk('3d', 1) };
  }

  makeBlit() {
    const code = `
      @group(0) @binding(0) var t : texture_2d<f32>;
      @group(0) @binding(1) var s : sampler;
      struct V { @builtin(position) p : vec4<f32>, @location(0) uv : vec2<f32> };
      @vertex fn vs(@builtin(vertex_index) i : u32) -> V {
        let uv = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
        return V(vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0), uv);
      }
      @fragment fn fs(v : V) -> @location(0) vec4<f32> { return vec4<f32>(textureSample(t, s, v.uv).rgb, 1.0); }`;
    const m = this.dev.createShaderModule({ code });
    this.blitPipes = new Map();
    this.blitModule = m;
    this.blitSampler = this.dev.createSampler({ magFilter: 'linear', minFilter: 'linear' });
  }
  blitPipe(format) {
    let p = this.blitPipes.get(format);
    if (!p) {
      p = this.dev.createRenderPipeline({ layout: 'auto', vertex: { module: this.blitModule, entryPoint: 'vs' },
        fragment: { module: this.blitModule, entryPoint: 'fs', targets: [{ format }] }, primitive: { topology: 'triangle-list' } });
      this.blitPipes.set(format, p);
    }
    return p;
  }
  blit(srcView, dstView, format, rect) {
    this.endPass();
    const pass = this.encoder.beginRenderPass({ colorAttachments: [{ view: dstView, loadOp: rect ? 'load' : 'clear', storeOp: 'store', clearValue: [0, 0, 0, 1] }] });
    const pipe = this.blitPipe(format);
    pass.setPipeline(pipe);
    if (rect) pass.setViewport(rect[0], rect[1], rect[2], rect[3], 0, 1);
    pass.setBindGroup(0, this.dev.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [{ binding: 0, resource: srcView }, { binding: 1, resource: this.blitSampler }] }));
    pass.draw(3);
    pass.end();
  }

  resize(w, h) {
    const old = this.textures.get(1);
    if (old && old.w === w && old.h === h) return;
    if (old) old.gpu.destroy();
    this.createTexture(1, 3, 22, w, h, 1, 1, 1);
    this.bbSize = [w, h];
    this.viewport = [0, 0, w, h, 0, 1];
  }

  // ---- resources ----------------------------------------------------------------------------------------------------
  createTexture(id, kind, fmt, w, h, depth, levels, usage) {
    const info = formatInfo(fmt);
    const isDepth = info.depth || kind === 4;
    let gpuFormat = isDepth ? 'depth24plus-stencil8' : info.gpu;
    if (info.block && ((w & 3) || (h & 3))) { gpuFormat = 'bgra8unorm'; info.badBlock = true; }
    if ((kind === 3 || (usage & 1)) && (info.block || info.conv && info.conv !== 'xrgb')) gpuFormat = 'bgra8unorm';
    let u = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC;
    if (kind === 3 || kind === 4 || (usage & 3) || isDepth) u |= GPUTextureUsage.RENDER_ATTACHMENT;
    if (gpuFormat.startsWith('bc')) u &= ~GPUTextureUsage.RENDER_ATTACHMENT;
    const layers = kind === 1 ? 6 : kind === 2 ? depth : 1;
    const maxLevels = Math.floor(Math.log2(Math.max(w, h, kind === 2 ? depth : 1))) + 1;
    let gpu;
    try {
      gpu = this.dev.createTexture({ size: [w, h, layers], format: gpuFormat, usage: u, mipLevelCount: Math.min(levels, maxLevels),
        dimension: kind === 2 ? '3d' : '2d' });
    } catch (e) {
      this.warn(`texture ${w}x${h} fmt ${fmt} (${gpuFormat}): ${e.message}`);
      return;
    }
    const t = { id, kind, fmt, w, h, depth, levels: Math.min(levels, maxLevels), info, gpuFormat, gpu, views: new Map(), isDepth };
    t.sampleView = gpu.createView({ dimension: kind === 1 ? 'cube' : kind === 2 ? '3d' : '2d', aspect: isDepth ? 'depth-only' : 'all' });
    this.textures.set(id, t);
    if (info.unknown && !isDepth && fmt !== FMT.NULL) this.warn('texture format ' + fmt + ' treated as BGRA8');
  }
  targetView(t, face, level) {
    const k = face * 64 + level;
    let v = t.views.get(k);
    if (!v) { v = t.gpu.createView({ dimension: '2d', baseMipLevel: level, mipLevelCount: 1, baseArrayLayer: face, arrayLayerCount: 1 }); t.views.set(k, v); }
    return v;
  }
  uploadTexture(w, data) {
    const [id, face, level, x, y, ww, hh, rowBytes, rows] = w;
    const t = this.textures.get(id);
    if (!t || level >= t.levels) return;
    if (this.used.has(id)) this.flush();
    const lw = Math.max(1, t.w >> level), lh = Math.max(1, t.h >> level);
    const info = t.info;
    let src = data, bpr = rowBytes, cw = ww, ch = hh;
    if (info.block && t.gpuFormat.startsWith('bc')) {
      cw = Math.max(4, (ww + 3) & ~3); ch = Math.max(4, (hh + 3) & ~3);
      // physical size of a small mip may exceed the level: WebGPU accepts the block-rounded extent
      if (x + cw > ((lw + 3) & ~3) || y + ch > ((lh + 3) & ~3)) return;
    } else if (info.block) {
      return;                                     // non-multiple-of-4 compressed texture: left black (rare)
    } else if (info.conv) {
      const slices = t.kind === 2 ? rows / hh : 1;
      if (slices !== 1) return;
      src = convertPixels(info.conv, data, ww, hh, rowBytes);
      bpr = ww * 4;
    }
    const depthOrLayers = t.kind === 2 ? Math.max(1, t.depth >> level) : 1;
    try {
      this.dev.queue.writeTexture({ texture: t.gpu, mipLevel: level, origin: [x, y, t.kind === 1 ? face : 0] }, src,
        { bytesPerRow: bpr, rowsPerImage: info.block ? ch / 4 : ch }, [cw, ch, depthOrLayers]);
    } catch (e) {
      this.warn('upload: ' + e.message);
    }
  }
  createBuffer(id, size, kind) {
    const old = this.buffers.get(id);
    if (old) old.gpu.destroy();
    const s = (size + 3) & ~3;
    const gpu = this.dev.createBuffer({ size: Math.max(s, 4), usage: GPUBufferUsage.VERTEX | GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
    this.buffers.set(id, { id, size, kind, gpu, cpu: new Uint8Array(Math.max(s, 4)) });
  }
  uploadBuffer(id, off, size, data) {
    const b = this.buffers.get(id);
    if (!b) return;
    if (this.used.has(id)) this.flush();
    b.cpu.set(data.subarray(0, Math.min(size, b.cpu.length - off)), off);
    const a = off & ~3, e = Math.min((off + size + 3) & ~3, b.cpu.length);
    this.dev.queue.writeBuffer(b.gpu, a, b.cpu, a, e - a);
  }
  destroy(id) {
    const t = this.textures.get(id);
    if (t && id !== 1) { this.endPass(); this.pendingDestroy = (this.pendingDestroy || []).concat([t.gpu]); this.textures.delete(id); this.bindGroups.clear(); }
    const b = this.buffers.get(id);
    if (b) { this.pendingDestroy = (this.pendingDestroy || []).concat([b.gpu]); this.buffers.delete(id); }
    this.shaders.delete(id);
  }
  createShader(id, stage, sha) {
    const sh = { id, stage, sha, info: null, wgsl: null };
    sh.ready = this.fetchShader(sha).then((j) => {
      if (!j) { this.warn('no WGSL for shader ' + sha); return; }
      sh.info = j.info; sh.name = j.name;
      // both stages bind from group 0 in the translation: the pixel stage moves to group 1
      let code = stage ? j.wgsl.replace(/@group\(0u?\)/g, '@group(1u)') : j.wgsl;
      // relative constant addressing (skinning: c[a0.x + n]) can reach any register, but the translation sizes the
      // array to the highest register it saw: give such shaders the whole register file
      const c = sh.info.bindings.constants;
      if (c && /tint_symbol\[\(/.test(code)) {
        const full = stage ? 224 : 256;
        code = code.replace(/(tint_symbol\s*:\s*array<vec4<f32>,\s*)\d+u?>/, `$1${full}u>`);
        sh.info = { ...sh.info, bindings: { ...sh.info.bindings, constants: { ...c, vec4_count: full } } };
      }
      sh.wgsl = code;
    });
    this.shaders.set(id, sh);
  }
  createDecl(id, n, bytes) {
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const el = [];
    for (let i = 0; i < n; i++) {
      const stream = dv.getUint16(i * 8, true);
      if (stream === 0xff) break;
      const type = dv.getUint8(i * 8 + 4);
      if (type === 17) continue;
      el.push({ stream, offset: dv.getUint16(i * 8 + 2, true), type, usage: dv.getUint8(i * 8 + 6), index: dv.getUint8(i * 8 + 7) });
    }
    this.decls.set(id, el);
  }

  // ---- passes -------------------------------------------------------------------------------------------------------
  endPass() { if (this.pass) { this.pass.end(); this.pass = null; } }
  attachments() {
    const colors = [];
    for (const r of this.rt) {
      if (!r || !r.id) { colors.push(null); continue; }
      const t = this.textures.get(r.id);
      if (!t || t.isDepth || t.fmt === FMT.NULL || !(t.gpu.usage & GPUTextureUsage.RENDER_ATTACHMENT)) { colors.push(null); continue; }
      colors.push({ t, view: this.targetView(t, r.face, r.level), w: Math.max(1, t.w >> r.level), h: Math.max(1, t.h >> r.level) });
    }
    while (colors.length && !colors[colors.length - 1]) colors.pop();
    let depth = null;
    const size = colors.find((c) => c) || null;
    if (this.depth) {
      const t = this.textures.get(this.depth);
      if (t && t.isDepth) {
        if (!size || (t.w === size.w && t.h === size.h)) depth = { t, view: this.targetView(t, 0, 0), w: t.w, h: t.h };
        else depth = this.substituteDepth(size.w, size.h);
      }
    }
    return { colors, depth, w: size ? size.w : depth ? depth.w : 1, h: size ? size.h : depth ? depth.h : 1 };
  }
  substituteDepth(w, h) {
    this.subDepth = this.subDepth || new Map();
    const k = w + 'x' + h;
    let t = this.subDepth.get(k);
    if (!t) {
      const gpu = this.dev.createTexture({ size: [w, h], format: 'depth24plus-stencil8', usage: GPUTextureUsage.RENDER_ATTACHMENT });
      t = { t: { id: -1, gpuFormat: 'depth24plus-stencil8' }, view: gpu.createView(), w, h };
      this.subDepth.set(k, t);
    }
    return t;
  }
  beginPass(clear) {
    this.endPass();
    const a = this.attachments();
    if (!a.colors.length && !a.depth) return null;
    const desc = {
      colorAttachments: a.colors.map((c) => c && { view: c.view, loadOp: clear && clear.color ? 'clear' : 'load', storeOp: 'store', clearValue: clear ? clear.colorValue : undefined }),
      depthStencilAttachment: a.depth ? {
        view: a.depth.view,
        depthLoadOp: clear && clear.z ? 'clear' : 'load', depthStoreOp: 'store', depthClearValue: clear ? clear.zValue : 1,
        stencilLoadOp: clear && clear.stencil ? 'clear' : 'load', stencilStoreOp: 'store', stencilClearValue: clear ? clear.stencilValue : 0,
      } : undefined,
    };
    this.pass = this.encoder.beginRenderPass(desc);
    this.passInfo = a;
    for (const c of a.colors) if (c) this.used.add(c.t.id);
    if (a.depth) this.used.add(a.depth.t.id);
    this.passState = {};
    return this.pass;
  }
  ensurePass() { return this.pass || this.beginPass(null); }

  clear(flags, color, z, stencil) {
    const a = this.attachments();
    const vp = this.viewport;
    const full = vp[0] <= 0 && vp[1] <= 0 && vp[0] + vp[2] >= a.w && vp[1] + vp[3] >= a.h;
    if (!full) this.warn(`partial clear ${vp.join(',')} of ${a.w}x${a.h}: treated as full`);
    const c = { color: !!(flags & 1), z: !!(flags & 2), stencil: !!(flags & 4),
      colorValue: [((color >>> 16) & 255) / 255, ((color >>> 8) & 255) / 255, (color & 255) / 255, (color >>> 24) / 255],
      zValue: Math.min(1, Math.max(0, z)), stencilValue: stencil & 255 };
    this.beginPass(c);
  }

  // ---- pipelines ----------------------------------------------------------------------------------------------------
  module(code, label) {
    let m = this.modules.get(code);
    if (!m) {
      m = this.dev.createShaderModule({ code, label });
      this.modules.set(code, m);
      m.getCompilationInfo?.().then((ci) => { for (const msg of ci.messages) if (msg.type === 'error') this.warn(`${label}: ${msg.message}`); });
    }
    return m;
  }
  stageLayout(sh, group, texKinds) {
    // group entries: constants (dynamic uniform) + texture/sampler pairs from the manifest
    const vis = sh.stage ? GPUShaderStage.FRAGMENT : GPUShaderStage.VERTEX;
    const entries = [];
    const c = sh.info.bindings.constants;
    if (c) entries.push({ binding: c.binding, visibility: vis, buffer: { type: 'uniform', hasDynamicOffset: true } });
    sh.info.bindings.samplers.forEach((s, i) => {
      const uf = texKinds[i] === 'u';
      entries.push({ binding: s.texture_binding, visibility: vis, texture: { sampleType: uf ? 'unfilterable-float' : 'float', viewDimension: s.dim } });
      entries.push({ binding: s.sampler_binding, visibility: vis, sampler: { type: uf ? 'non-filtering' : 'filtering' } });
    });
    const key = group + ':' + JSON.stringify(entries);
    let l = this.layouts.get(key);
    if (!l) { l = this.dev.createBindGroupLayout({ entries }); this.layouts.set(key, l); }
    return l;
  }
  vertexState(vs, elements, instanced) {
    // one buffer layout per stream the shader reads; inputs with no element read a zero buffer
    const streams = new Map();
    const patches = new Map();
    let zeroSlot = false;
    const locs = [];
    for (const inp of vs.info.inputs) {
      if (inp.location == null) continue;
      const el = elements.find((e) => semanticsOf(e.usage, e.index).includes(inp.semantic));
      if (!el) { locs.push({ loc: inp.location, zero: true }); zeroSlot = true; continue; }
      const t = DECL_TYPES[el.type] || DECL_TYPES[3];
      if (t[2] !== 'f32') patches.set(inp.location, t[2]);
      if (!streams.has(el.stream)) streams.set(el.stream, []);
      streams.get(el.stream).push({ shaderLocation: inp.location, offset: el.offset, format: t[0] });
    }
    const order = [...streams.keys()].sort((a, b) => a - b);
    const buffers = order.map((s) => {
      const st = this.streams[s];
      const stepMode = instanced && (st.freq & 0x80000000) ? 'instance' : 'vertex';
      let stride = st.stride;
      const attrs = streams.get(s);
      const need = Math.max(...attrs.map((a) => a.offset + (DECL_TYPES.find((d) => d[0] === a.format) || [0, 16])[1]));
      if (stride && stride < need) stride = (need + 3) & ~3;
      return { arrayStride: stride & ~3, stepMode, attributes: attrs };
    });
    if (zeroSlot) buffers.push({ arrayStride: 0, stepMode: 'vertex', attributes: locs.filter((l) => l.zero).map((l) => ({ shaderLocation: l.loc, offset: 0, format: 'float32x4' })) });
    return { buffers, order, zeroSlot, patches };
  }
  vsCode(vs, patches) {
    if (!patches.size) return vs.wgsl;
    const e = splitEntry(vs.wgsl, 'vertex');
    const pre = [];
    const params = e.params.map((p) => {
      const base = patches.get(p.location);
      if (base == null) return `${p.attrs} ${p.name} : ${p.type}`;
      const ty = p.type.replace('f32', base);
      pre.push(`  let ${p.name} = ${p.type}(${p.name}_in);`);
      return `${p.attrs} ${p.name}_in : ${ty}`;
    });
    return joinEntry(e, params, pre);
  }
  psCode(ps, vs, alpha) {
    const outs = new Map();
    for (const o of vs.info.outputs) if (o.location != null) outs.set(o.location, o.type);
    const e = splitEntry(ps.wgsl, 'fragment');
    const pre = [];
    const params = [];
    for (const p of e.params) {
      const attrs = p.attrs.replace(/@interpolate\([^)]*\)/g, '').trim();
      if (p.location == null) { params.push(`${attrs} ${p.name} : ${p.type}`); continue; }
      const vt = outs.get(p.location);
      if (!vt) { pre.push(`  let ${p.name} = ${p.type}();`); continue; }
      if (vt === p.type) { params.push(`${attrs} ${p.name} : ${p.type}`); continue; }
      params.push(`${attrs} ${p.name}_in : ${vt}`);
      pre.push(`  let ${p.name} = ${convertExpr(p.name + '_in', vt, p.type)};`);
    }
    let code = joinEntry(e, params, pre);
    // debugging: renderer.debugPs = {name substring: 'vec4<f32>(...)'} replaces the colour those shaders write
    for (const [k, v] of Object.entries(this.debugPs || {}))
      if (ps.name.includes(k)) code = code.replace(/(\n\s*main_inner\([^;]*\);)/, `$1\n  o0 = ${v};`);
    if (alpha) {
      const [func, ref] = alpha;
      const op = { 1: 'false', 2: 'a < r', 3: 'a == r', 4: 'a <= r', 5: 'a > r', 6: 'a != r', 7: 'a >= r', 8: 'true' }[func] || 'true';
      code = code.replace(/(\n\s*main_inner\([^;]*\);)/, `$1\n  { let a = o0.w; let r = ${(ref / 255).toFixed(6)}; if (!(${op})) { discard; } }`);
    }
    return code;
  }
  blendState(i) {
    const rs = this.rs;
    if (!rs[RS.ALPHABLENDENABLE]) return undefined;
    const cs = BLEND[rs[RS.SRCBLEND]] || 'one', cd = BLEND[rs[RS.DESTBLEND]] || 'zero';
    let src = cs, dst = cd;
    if (rs[RS.SRCBLEND] === 12) { src = 'src-alpha'; dst = 'one-minus-src-alpha'; }
    if (rs[RS.SRCBLEND] === 13) { src = 'one-minus-src-alpha'; dst = 'src-alpha'; }
    const op = BLENDOP[rs[RS.BLENDOP]] || 'add';
    const color = { srcFactor: src, dstFactor: dst, operation: op };
    let alpha = { srcFactor: BLEND_ALPHA(src), dstFactor: BLEND_ALPHA(dst), operation: op };
    if (rs[RS.SEPARATEALPHABLENDENABLE]) {
      alpha = { srcFactor: BLEND_ALPHA(BLEND[rs[RS.SRCBLENDALPHA]] || 'one'), dstFactor: BLEND_ALPHA(BLEND[rs[RS.DESTBLENDALPHA]] || 'zero'), operation: BLENDOP[rs[RS.BLENDOPALPHA]] || 'add' };
    }
    for (const b of [color, alpha]) if (b.operation === 'min' || b.operation === 'max') { b.srcFactor = 'one'; b.dstFactor = 'one'; }
    return { color, alpha };
  }
  depthStencil(fmt) {
    if (!fmt) return undefined;
    const rs = this.rs;
    const z = rs[RS.ZENABLE];
    const face = (f, zf, p, fn) => ({ compare: CMP[rs[fn]] || 'always', failOp: STENCILOP[rs[f]] || 'keep', depthFailOp: STENCILOP[rs[zf]] || 'keep', passOp: STENCILOP[rs[p]] || 'keep' });
    const stencil = !!rs[RS.STENCILENABLE];
    const front = stencil ? face(RS.STENCILFAIL, RS.STENCILZFAIL, RS.STENCILPASS, RS.STENCILFUNC) : {};
    const back = stencil ? (rs[RS.TWOSIDEDSTENCILMODE] ? face(RS.CCW_STENCILFAIL, RS.CCW_STENCILZFAIL, RS.CCW_STENCILPASS, RS.CCW_STENCILFUNC) : front) : {};
    return {
      format: fmt, depthWriteEnabled: !!(z && rs[RS.ZWRITEENABLE]), depthCompare: z ? CMP[rs[RS.ZFUNC]] || 'less-equal' : 'always',
      stencilFront: front, stencilBack: back,
      stencilReadMask: stencil ? rs[RS.STENCILMASK] & 255 : 0xff, stencilWriteMask: stencil ? rs[RS.STENCILWRITEMASK] & 255 : 0,
      depthBias: Math.round(new Float32Array(new Uint32Array([rs[RS.DEPTHBIAS]]).buffer)[0] * 16777215),
      depthBiasSlopeScale: new Float32Array(new Uint32Array([rs[RS.SLOPESCALEDEPTHBIAS]]).buffer)[0],
    };
  }

  // ---- draws --------------------------------------------------------------------------------------------------------
  async draw(kind, w, data) {
    const vs = this.shaders.get(this.vs), ps = this.shaders.get(this.ps);
    if (!vs || !ps) { this.stats.skipped++; this.warn('draw without shaders (fixed function) skipped'); return; }
    if (!vs.wgsl) await vs.ready;
    if (!ps.wgsl) await ps.ready;
    if (!vs.wgsl || !ps.wgsl) { this.stats.skipped++; return; }
    const prim = w[0];
    let count, start = 0, baseVertex = 0, indexed = false;
    // primitive count -> vertex/index count
    const n = (pc) => [0, pc, pc * 2, pc + 1, pc * 3, pc + 2, pc + 2][prim] || 0;
    let upBuffer = null, upOffset = 0, upIndexOffset = 0, upIndex32 = false, upStride = 0;
    if (kind === OP.DRAW) { start = w[1]; count = n(w[2]); }
    else if (kind === OP.DRAW_INDEXED) { baseVertex = w[1]; start = w[4]; count = n(w[5]); indexed = true; }
    else if (kind === OP.DRAW_UP) {
      count = n(w[1]); upStride = w[2];
      upOffset = this.arenaPut(data.subarray(0, w[3]));
    } else {
      // min index, vertex count, primitive count, index32, stride, index bytes, vertex bytes, data
      count = n(w[3]); upIndex32 = !!w[4]; upStride = w[5];
      const ib = w[6], vb = w[7];
      upIndexOffset = this.arenaPut(data.subarray(0, ib));
      upOffset = this.arenaPut(data.subarray((ib + 3) & ~3, ((ib + 3) & ~3) + vb));
      if (data.length < ((ib + 3) & ~3) + vb) upOffset = this.arenaPut(data.subarray(ib, ib + vb));
      indexed = true;
    }
    if (!count) return;
    const isUP = kind === OP.DRAW_UP || kind === OP.DRAW_INDEXED_UP;
    let elements = this.decl ? this.decls.get(this.decl) : fvfElements(this.fvf);
    if (!elements) return;
    const savedStream0 = this.streams[0];
    if (isUP) this.streams[0] = { id: -2, offset: upOffset, stride: upStride, freq: 1 };
    const instanced = (this.streams[0].freq & 0x40000000) !== 0;
    const instances = instanced ? (this.streams[0].freq & 0xffff) || 1 : 1;
    const vstate = this.vertexState(vs, elements, instanced);

    // topology (fans become lists through an index buffer built here)
    let topology = ['point-list', 'point-list', 'line-list', 'line-strip', 'triangle-list', 'triangle-strip', 'triangle-list'][prim];
    let fanIndex = null;
    if (prim === 6) {
      const tris = count - 2;
      const out = new Uint32Array(tris * 3);
      let src = null;
      if (indexed) {
        const ib = isUP ? null : this.buffers.get(this.indices);
        if (isUP) {
          const raw = data.subarray(0, w[6]);
          src = upIndex32 ? new Uint32Array(raw.slice().buffer) : new Uint16Array(raw.slice().buffer);
        } else if (ib) src = ib.kind === 2 ? new Uint32Array(ib.cpu.buffer, 0, ib.cpu.length >> 2) : new Uint16Array(ib.cpu.buffer, 0, ib.cpu.length >> 1);
      }
      for (let i = 0; i < tris; i++) {
        const a = start, b = start + i + 1, c = start + i + 2;
        out[i * 3] = src ? src[a] : a; out[i * 3 + 1] = src ? src[b] : b; out[i * 3 + 2] = src ? src[c] : c;
      }
      fanIndex = this.arenaPut(new Uint8Array(out.buffer));
      count = tris * 3;
    }
    const a = this.passInfo && this.pass ? this.passInfo : this.attachments();
    const colorFormats = a.colors.map((c) => c && c.t.gpuFormat);
    const depthFormat = a.depth ? 'depth24plus-stencil8' : undefined;
    if (!colorFormats.length && !depthFormat) return;

    // textures, samplers
    const texKinds = [];
    const texViews = [];
    const samplers = [];
    for (const sh of [vs, ps]) {
      for (const s of sh.info.bindings.samplers) {
        const stage = sh.stage ? s.n : 16 + s.n;
        const t = this.textures.get(this.tex[stage]);
        let view = this.dummy[s.dim] || this.dummy['2d'];
        let uf = false;
        const inUse = t && a.colors.some((c) => c && c.t === t) || (t && a.depth && a.depth.t === t);
        const dimOk = t && ((s.dim === 'cube') === (t.kind === 1)) && ((s.dim === '3d') === (t.kind === 2));
        if (t && !inUse && dimOk) { view = t.sampleView; uf = !!(t.info.unfilterable || t.isDepth); }
        texKinds.push(uf ? 'u' : 'f');
        texViews.push(view);
        samplers.push(this.sampler(stage, uf));
        if (t) this.used.add(t.id);
      }
    }
    const vsKinds = texKinds.slice(0, vs.info.bindings.samplers.length), psKinds = texKinds.slice(vs.info.bindings.samplers.length);
    const rs = this.rs;
    const alpha = rs[RS.ALPHATESTENABLE] && rs[RS.ALPHAFUNC] !== 8 ? [rs[RS.ALPHAFUNC], rs[RS.ALPHAREF] & 255] : null;
    const cull = rs[RS.CULLMODE] === 1 ? 'none' : 'back';
    const frontFace = rs[RS.CULLMODE] === 3 ? 'cw' : 'ccw';
    const writeMasks = [RS.COLORWRITEENABLE, 190, 191, 192].map((r) => rs[r] & 15);
    const blend = this.blendState();
    const key = [this.vs, this.ps, JSON.stringify(vstate.buffers), topology, indexed && topology === 'triangle-strip' ? (isUP ? (upIndex32 ? 32 : 16) : (this.buffers.get(this.indices)?.kind === 2 ? 32 : 16)) : 0,
      JSON.stringify(blend), cull, frontFace, writeMasks.join(), JSON.stringify(this.depthStencil(depthFormat)), colorFormats.join(), alpha && alpha.join(), texKinds.join('')].join('|');
    let pipe = this.pipelines.get(key);
    if (pipe === undefined) {
      try {
        const vsl = this.stageLayout(vs, 0, vsKinds), psl = this.stageLayout(ps, 1, psKinds);
        const layout = this.dev.createPipelineLayout({ bindGroupLayouts: [vsl, psl] });
        const stripFormat = topology === 'triangle-strip' || topology === 'line-strip' ? (indexed ? (key.split('|')[4] === '32' ? 'uint32' : 'uint16') : undefined) : undefined;
        this.dev.pushErrorScope('validation');
        const p = this.dev.createRenderPipeline({
          layout,
          vertex: { module: this.module(this.vsCode(vs, vstate.patches), vs.name), entryPoint: 'main', buffers: vstate.buffers },
          fragment: { module: this.module(this.psCode(ps, vs, alpha), ps.name), entryPoint: 'main',
            targets: colorFormats.map((f, i) => f ? { format: f, blend: f.includes('32float') ? undefined : blend, writeMask: writeMasks[i] } : null) },
          primitive: { topology, cullMode: cull, frontFace, stripIndexFormat: fanIndex != null ? undefined : stripFormat },
          depthStencil: this.depthStencil(depthFormat),
        });
        pipe = { p, vsl, psl };
        this.pipelines.set(key, pipe);
        this.dev.popErrorScope().then((err) => { if (err) { this.warn(`pipeline ${vs.name} + ${ps.name}: ${err.message.slice(0, 300)}`); this.pipelines.set(key, null); } });
      } catch (e) {
        this.warn(`pipeline ${vs.name} + ${ps.name}: ${e.message}`);
        this.pipelines.set(key, null);
        pipe = null;
      }
    }
    if (!pipe) { this.stats.skipped++; if (isUP) this.streams[0] = savedStream0; return; }

    const pass = this.ensurePass();
    if (!pass) return;
    pass.setPipeline(pipe.p);
    // viewport / scissor
    const vp = this.viewport;
    const vx = Math.max(0, vp[0]), vy = Math.max(0, vp[1]);
    const vw = Math.max(1, Math.min(vp[2], a.w - vx)), vh = Math.max(1, Math.min(vp[3], a.h - vy));
    pass.setViewport(vx, vy, vw, vh, Math.min(vp[4], vp[5]), Math.max(vp[4], vp[5]));
    if (rs[RS.SCISSORTESTENABLE]) {
      const [l, t, r, b] = this.scissor;
      const x0 = Math.max(0, Math.min(l, a.w)), y0 = Math.max(0, Math.min(t, a.h));
      pass.setScissorRect(x0, y0, Math.max(0, Math.min(r, a.w) - x0), Math.max(0, Math.min(b, a.h) - y0));
    } else pass.setScissorRect(0, 0, a.w, a.h);
    if (depthFormat) pass.setStencilReference(rs[RS.STENCILREF] & 255);
    const bf = rs[RS.BLENDFACTOR];
    pass.setBlendConstant([((bf >>> 16) & 255) / 255, ((bf >>> 8) & 255) / 255, (bf & 255) / 255, (bf >>> 24) / 255]);
    // bind groups
    let ti = 0;
    for (const [g, sh, layout] of [[0, vs, pipe.vsl], [1, ps, pipe.psl]]) {
      const entries = [];
      const c = sh.info.bindings.constants;
      const offsets = [];
      if (c) {
        const bytes = c.vec4_count * 16;
        offsets.push(this.uniformPut(sh.stage ? this.psConst : this.vsConst, bytes));
        entries.push({ binding: c.binding, resource: { buffer: this.uniformArena, offset: 0, size: Math.max(16, bytes) } });
      }
      for (const s of sh.info.bindings.samplers) {
        entries.push({ binding: s.texture_binding, resource: texViews[ti] });
        entries.push({ binding: s.sampler_binding, resource: samplers[ti] });
        ti++;
      }
      pass.setBindGroup(g, this.dev.createBindGroup({ layout, entries }), offsets);
    }
    // vertex buffers
    let slot = 0;
    for (const s of vstate.order) {
      const st = this.streams[s];
      if (st.id === -2) pass.setVertexBuffer(slot, this.vertexArena, st.offset);
      else {
        const b = this.buffers.get(st.id);
        if (!b) { this.stats.skipped++; if (isUP) this.streams[0] = savedStream0; return; }
        this.used.add(b.id);
        pass.setVertexBuffer(slot, b.gpu, st.offset & ~3);
      }
      slot++;
    }
    if (vstate.zeroSlot) pass.setVertexBuffer(slot, this.zeroVB);
    if (isUP) this.streams[0] = savedStream0;
    this.stats.draws++;
    if (this.trace) this.trace.push({ vs: vs.name, ps: ps.name, prim, count, rt: this.rt.map((r) => r && r.id), depth: this.depth,
      tex: texViews.map((v, i) => { const st = i < vs.info.bindings.samplers.length ? 16 + vs.info.bindings.samplers[i].n : ps.info.bindings.samplers[i - vs.info.bindings.samplers.length].n; const tx = this.textures.get(this.tex[st]); return tx ? `${tx.id}:${tx.fmt}:${tx.w}x${tx.h}${v === tx.sampleView ? '' : '(dummy)'}` : 'none'; }),
      blend: rs[RS.ALPHABLENDENABLE] ? `${rs[RS.SRCBLEND]}/${rs[RS.DESTBLEND]}` : 'off', alpha, z: `${rs[RS.ZENABLE]}${rs[RS.ZWRITEENABLE]}${rs[RS.ZFUNC]}`, stencil: rs[RS.STENCILENABLE] });
    if (fanIndex != null) {
      pass.setIndexBuffer(this.vertexArena, 'uint32', fanIndex);
      pass.drawIndexed(count, instances, 0, indexed && !isUP ? baseVertex : 0, 0);
    } else if (kind === OP.DRAW_INDEXED_UP) {
      pass.setIndexBuffer(this.vertexArena, upIndex32 ? 'uint32' : 'uint16', upIndexOffset);
      pass.drawIndexed(count, 1, 0, 0, 0);
    } else if (indexed) {
      const ib = this.buffers.get(this.indices);
      if (!ib) return;
      this.used.add(ib.id);
      pass.setIndexBuffer(ib.gpu, ib.kind === 2 ? 'uint32' : 'uint16');
      pass.drawIndexed(count, instances, start, baseVertex, 0);
    } else pass.draw(count, instances, start, 0);
  }

  sampler(stage, unfilterable) {
    const s = this.ss[stage] || this.ss[0];
    const key = [s[1], s[2], s[3], s[5], s[6], s[7], s[10], unfilterable].join();
    let smp = this.samplers.get(key);
    if (!smp) {
      const f = (v) => (unfilterable ? 'nearest' : v >= 2 ? 'linear' : 'nearest');
      const aniso = !unfilterable && (s[5] === 3 || s[6] === 3) && s[7] === 2 ? Math.max(1, Math.min(16, s[10])) : 1;
      smp = this.dev.createSampler({ addressModeU: ADDRESS[s[1]] || 'repeat', addressModeV: ADDRESS[s[2]] || 'repeat', addressModeW: ADDRESS[s[3]] || 'repeat',
        magFilter: aniso > 1 ? 'linear' : f(s[5]), minFilter: aniso > 1 ? 'linear' : f(s[6]), mipmapFilter: s[7] === 2 && !unfilterable ? 'linear' : 'nearest',
        lodMaxClamp: s[7] === 0 ? 0 : 32, maxAnisotropy: aniso });
      this.samplers.set(key, smp);
    }
    return smp;
  }

  arenaPut(bytes) {
    const n = (bytes.length + 3) & ~3;
    if (this.vertexUsed + n > this.vertexStage.length) this.flush();
    const off = this.vertexUsed;
    this.vertexStage.set(bytes, off);
    this.vertexUsed += n;
    return off;
  }
  uniformPut(arr, bytes) {
    const n = (Math.max(16, bytes) + 255) & ~255;
    if (this.uniformUsed + n > this.uniformStage.length) this.flush();
    const off = this.uniformUsed;
    this.uniformStage.set(new Uint8Array(arr.buffer, 0, Math.min(bytes, arr.byteLength)), off);
    this.uniformUsed += n;
    return off;
  }

  flush() {
    this.endPass();
    if (this.uniformUsed) this.dev.queue.writeBuffer(this.uniformArena, 0, this.uniformStage, 0, this.uniformUsed);
    if (this.vertexUsed) this.dev.queue.writeBuffer(this.vertexArena, 0, this.vertexStage, 0, this.vertexUsed);
    this.dev.queue.submit([this.encoder.finish()]);
    this.stats.submits++;
    this.encoder = this.dev.createCommandEncoder();
    this.uniformUsed = 0; this.vertexUsed = 0;
    this.used.clear();
    if (this.pendingDestroy) { for (const g of this.pendingDestroy) g.destroy(); this.pendingDestroy = null; }
  }

  // debugging: the draws of the next whole frame
  traceFrame() {
    return new Promise((resolve) => { this.traceWant = resolve; });
  }

  present() {
    if (this.trace && this.traceWant) { const r = this.traceWant; this.traceWant = null; r(this.trace); this.trace = null; }
    else if (this.traceWant && !this.trace) this.trace = [];
    const bb = this.textures.get(1);
    this.endPass();
    if (bb) {
      if (this.canvas.width !== bb.w || this.canvas.height !== bb.h) { this.canvas.width = bb.w; this.canvas.height = bb.h; }
      this.blit(bb.sampleView, this.ctx.getCurrentTexture().createView(), this.canvasFormat);
    }
    this.flush();
    this.frames++;
  }

  // the back buffer as a PNG blob (debugging: index.html posts it to the dev server's /snap)
  async capture() {
    const bb = this.textures.get(1);
    const row = (bb.w * 4 + 255) & ~255;
    const buf = this.dev.createBuffer({ size: row * bb.h, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    this.endPass();
    this.encoder.copyTextureToBuffer({ texture: bb.gpu }, { buffer: buf, bytesPerRow: row }, [bb.w, bb.h, 1]);
    this.flush();
    await buf.mapAsync(GPUMapMode.READ);
    const src = new Uint8Array(buf.getMappedRange());
    const img = new ImageData(bb.w, bb.h);
    for (let y = 0; y < bb.h; y++)
      for (let x = 0; x < bb.w; x++) {
        const s = y * row + x * 4, d = (y * bb.w + x) * 4;
        img.data[d] = src[s + 2]; img.data[d + 1] = src[s + 1]; img.data[d + 2] = src[s]; img.data[d + 3] = 255;
      }
    buf.unmap(); buf.destroy();
    const c = new OffscreenCanvas(bb.w, bb.h);
    c.getContext('2d').putImageData(img, 0, 0);
    return c.convertToBlob({ type: 'image/png' });
  }

  stretch(w) {
    const [si, sf, sl, , , , , di, df, dl, dx0, dy0, dx1, dy1] = w;
    const s = this.textures.get(si), d = this.textures.get(di);
    if (!s || !d || s.isDepth || d.isDepth) return;
    this.used.add(si); this.used.add(di);
    const sw = Math.max(1, s.w >> sl), sh = Math.max(1, s.h >> sl), dw = Math.max(1, d.w >> dl), dh = Math.max(1, d.h >> dl);
    const full = (dx1 | 0) === -1 || dx1 === 0xffffffff;
    const rect = full ? null : [dx0, dy0, dx1 - dx0, dy1 - dy0];
    if (!rect && sw === dw && sh === dh && s.gpuFormat === d.gpuFormat) {
      this.endPass();
      this.encoder.copyTextureToTexture({ texture: s.gpu, mipLevel: sl, origin: [0, 0, sf] }, { texture: d.gpu, mipLevel: dl, origin: [0, 0, df] }, [sw, sh, 1]);
      return;
    }
    if (!(d.gpu.usage & GPUTextureUsage.RENDER_ATTACHMENT)) return;
    const view = s.gpu.createView({ dimension: '2d', baseMipLevel: sl, mipLevelCount: 1, baseArrayLayer: sf, arrayLayerCount: 1 });
    this.blit(view, this.targetView(d, df, dl), d.gpuFormat, rect);
  }

  colorFill(w) {
    const [id, face, level, l, t, r, b, color] = w;
    const tex = this.textures.get(id);
    if (!tex || tex.isDepth || !(tex.gpu.usage & GPUTextureUsage.RENDER_ATTACHMENT)) return;
    this.endPass();
    this.used.add(id);
    const pass = this.encoder.beginRenderPass({ colorAttachments: [{ view: this.targetView(tex, face, level), loadOp: 'clear', storeOp: 'store',
      clearValue: [((color >>> 16) & 255) / 255, ((color >>> 8) & 255) / 255, (color & 255) / 255, (color >>> 24) / 255] }] });
    pass.end();
  }

  // ---- packet dispatch ----------------------------------------------------------------------------------------------
  // w: Uint32Array over the payload, data: Uint8Array over the payload (both may be views of shared memory)
  async exec(op, w, data) {
    switch (op) {
      case OP.CREATE_TEXTURE: this.createTexture(w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]); break;
      case OP.DESTROY: this.destroy(w[0]); break;
      case OP.UPLOAD_TEXTURE: this.uploadTexture(w, data.slice(36)); break;
      case OP.CREATE_BUFFER: this.createBuffer(w[0], w[1], w[2]); break;
      case OP.UPLOAD_BUFFER: this.uploadBuffer(w[0], w[1], w[2], data.subarray(12)); break;
      case OP.CREATE_SHADER: this.createShader(w[0], w[1], [...w.subarray(2, 7)].map((x) => x.toString(16).padStart(8, '0')).join('')); break;
      case OP.CREATE_DECL: this.createDecl(w[0], w[1], data.slice(8)); break;
      case OP.RENDER_STATE: if (w[0] < 256) this.rs[w[0]] = w[1]; break;
      case OP.SAMPLER_STATE: if (w[0] < 20 && w[1] < 14) this.ss[w[0]][w[1]] = w[2]; break;
      case OP.SET_TEXTURE: if (w[0] < 20) this.tex[w[0]] = w[1]; break;
      case OP.SET_SHADER: if (w[0]) this.ps = w[1]; else this.vs = w[1]; break;
      case OP.SHADER_CONST: {
        const arr = w[0] ? this.psConst : this.vsConst;
        const f = new Float32Array(data.slice(12, 12 + w[2] * 16).buffer);
        arr.set(f.subarray(0, Math.max(0, Math.min(f.length, arr.length - w[1] * 4))), w[1] * 4);
        break;
      }
      case OP.SET_STREAM: if (w[0] < 16) { const st = this.streams[w[0]]; st.id = w[1]; st.offset = w[2]; st.stride = w[3]; } break;
      case OP.STREAM_FREQ: if (w[0] < 16) this.streams[w[0]].freq = w[1]; break;
      case OP.SET_INDICES: this.indices = w[0]; break;
      case OP.SET_DECL: this.decl = w[0]; break;
      case OP.SET_FVF: this.fvf = w[0]; this.decl = 0; break;
      case OP.SET_TARGET: {
        const cur = this.rt[w[0]];
        const nw = w[1] ? { id: w[1], face: w[2], level: w[3] } : null;
        if (JSON.stringify(cur) !== JSON.stringify(nw)) { this.endPass(); this.rt[w[0]] = nw; }
        if (w[0] === 0 && nw) { const t = this.textures.get(nw.id); if (t) { this.viewport = [0, 0, Math.max(1, t.w >> nw.level), Math.max(1, t.h >> nw.level), 0, 1]; } }
        break;
      }
      case OP.SET_DEPTH: if (this.depth !== w[0]) { this.endPass(); this.depth = w[0]; } break;
      case OP.VIEWPORT: {
        const f = new Float32Array(w.slice(4, 6).buffer);
        this.viewport = [w[0], w[1], w[2], w[3], f[0], f[1]];
        break;
      }
      case OP.SCISSOR: this.scissor = [w[0] | 0, w[1] | 0, w[2] | 0, w[3] | 0]; break;
      case OP.CLEAR: if (this.trace) this.trace.push({ clear: w[0], color: w[1].toString(16), rt: this.rt.map((r) => r && r.id), vp: this.viewport.slice(0, 4) });
        this.clear(w[0], w[1], new Float32Array(w.slice(2, 3).buffer)[0], w[3]); break;
      case OP.DRAW: case OP.DRAW_INDEXED: await this.draw(op, w, null); break;
      case OP.DRAW_UP: await this.draw(op, w, data.subarray(16)); break;
      case OP.DRAW_INDEXED_UP: await this.draw(op, w, data.subarray(32)); break;
      case OP.STRETCH: this.stretch(w); break;
      case OP.COLOR_FILL: this.colorFill(w); break;
      case OP.PRESENT: this.present(); return true;
      case OP.RESIZE: this.endPass(); this.resize(w[0], w[1]); break;
      case OP.TEX_STAGE_STATE: case OP.SET_TRANSFORM: break;
      default: this.warn('unknown op ' + op);
    }
    return false;
  }
}

// GPU device with the features the game's formats need
export async function createDevice() {
  if (!navigator.gpu) throw new Error('WebGPU is not available in this browser');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('no WebGPU adapter');
  const want = ['texture-compression-bc', 'float32-filterable', 'depth-clip-control'];
  const requiredFeatures = want.filter((f) => adapter.features.has(f));
  const device = await adapter.requestDevice({ requiredFeatures, requiredLimits: {
    maxBindGroups: Math.min(4, adapter.limits.maxBindGroups),
    maxSampledTexturesPerShaderStage: adapter.limits.maxSampledTexturesPerShaderStage,
    maxSamplersPerShaderStage: adapter.limits.maxSamplersPerShaderStage,
    maxUniformBufferBindingSize: adapter.limits.maxUniformBufferBindingSize,
    maxBufferSize: adapter.limits.maxBufferSize,
  } });
  return { adapter, device, features: requiredFeatures };
}
