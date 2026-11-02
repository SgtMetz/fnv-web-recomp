import { fileSource, urlSource, openBsa, readFile, parseDds } from './bsa.js';

const $ = (id) => document.getElementById(id);
const BSA_NAME = 'Fallout - Textures.bsa';
let bsa, gpu, shown = [];

function status(s) { $('status').textContent = s; }

async function initGpu() {
  const adapter = await navigator.gpu?.requestAdapter();
  if (!adapter) throw new Error('WebGPU not available');
  if (!adapter.features.has('texture-compression-bc')) throw new Error('adapter lacks texture-compression-bc');
  const device = await adapter.requestDevice({ requiredFeatures: ['texture-compression-bc'] });
  const ctx = $('view').getContext('webgpu');
  const format = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format, alphaMode: 'opaque' });
  const module = device.createShaderModule({ code: `
    struct U { scale: vec2f, alpha: f32, pad: f32 };
    @group(0) @binding(0) var t: texture_2d<f32>;
    @group(0) @binding(1) var s: sampler;
    @group(0) @binding(2) var<uniform> u: U;
    struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
    @vertex fn vs(@builtin(vertex_index) i: u32) -> VO {
      let c = array(vec2f(-1,-1), vec2f(1,-1), vec2f(-1,1), vec2f(1,1))[i];
      return VO(vec4f(c * u.scale, 0, 1), vec2f(c.x * .5 + .5, .5 - c.y * .5));
    }
    @fragment fn fs(v: VO) -> @location(0) vec4f {
      let c = textureSample(t, s, v.uv);
      let p = vec2u(v.pos.xy / 12.0);
      let checker = select(0.18, 0.26, (p.x + p.y) % 2u == 0u);
      return vec4f(mix(vec3f(checker), c.rgb, c.a), 1);
    }` });
  const pipeline = device.createRenderPipeline({
    layout: 'auto', vertex: { module, entryPoint: 'vs' },
    fragment: { module, entryPoint: 'fs', targets: [{ format }] },
    primitive: { topology: 'triangle-strip' },
  });
  const sampler = device.createSampler({ magFilter: 'linear', minFilter: 'linear', mipmapFilter: 'linear' });
  const ubo = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  return { device, ctx, pipeline, sampler, ubo, tex: null, dims: null };
}

function draw() {
  if (!gpu?.tex) return;
  const { device, ctx, pipeline, sampler, ubo } = gpu;
  const cv = $('view'), dpr = devicePixelRatio;
  cv.width = Math.max(1, cv.clientWidth * dpr); cv.height = Math.max(1, cv.clientHeight * dpr);
  const [w, h] = gpu.dims, k = Math.min(cv.width * .92 / w, cv.height * .92 / h, 4);
  device.queue.writeBuffer(ubo, 0, new Float32Array([w * k / cv.width, h * k / cv.height, 1, 0]));
  const bind = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [
    { binding: 0, resource: gpu.tex.createView() }, { binding: 1, resource: sampler }, { binding: 2, resource: { buffer: ubo } }] });
  const enc = device.createCommandEncoder();
  const pass = enc.beginRenderPass({ colorAttachments: [{ view: ctx.getCurrentTexture().createView(),
    loadOp: 'clear', storeOp: 'store', clearValue: { r: .07, g: .07, b: .06, a: 1 } }] });
  pass.setPipeline(pipeline); pass.setBindGroup(0, bind); pass.draw(4); pass.end();
  device.queue.submit([enc.finish()]);
}

async function show(f, row) {
  document.querySelectorAll('#list .sel').forEach((e) => e.classList.remove('sel'));
  row?.classList.add('sel');
  const t0 = performance.now();
  try {
    const bytes = await readFile(bsa, f);
    const t1 = performance.now();
    const dds = parseDds(bytes);
    const blockFmt = dds.format.startsWith('bc');
    if (blockFmt && (dds.width % 4 || dds.height % 4)) throw new Error('BC texture size not a multiple of 4');
    gpu ??= await initGpu();
    gpu.tex?.destroy();
    gpu.tex = gpu.device.createTexture({ size: [dds.width, dds.height], format: dds.format, mipLevelCount: dds.mips.length,
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
    dds.mips.forEach((m, level) => gpu.device.queue.writeTexture({ texture: gpu.tex, mipLevel: level }, m.data,
      { bytesPerRow: m.bytesPerRow, rowsPerImage: m.rowsPerImage }, [m.width, m.height]));
    gpu.dims = [dds.width, dds.height];
    draw();
    $('info').textContent = `${f.path}  ·  ${dds.width}×${dds.height} ${dds.fourCC} → ${dds.format}, ${dds.mips.length} mips  ·  `
      + `${(bytes.length / 1024).toFixed(0)} KiB, read+inflate ${(t1 - t0).toFixed(1)} ms`;
    window.__lastShown = { path: f.path, width: dds.width, height: dds.height, format: dds.format, mips: dds.mips.length };
  } catch (e) {
    $('info').textContent = `${f.path}: ${e.message}`;
    window.__lastShown = { path: f.path, error: e.message };
  }
}

function renderList() {
  const q = $('filter').value.toLowerCase().replace(/\//g, '\\');
  shown = bsa.files.filter((f) => f.path.endsWith('.dds') && f.path.includes(q)).slice(0, 2000);
  const list = $('list');
  list.replaceChildren(...shown.map((f) => {
    const d = document.createElement('div');
    d.textContent = f.path; d.title = f.path; d.onclick = () => show(f, d);
    return d;
  }));
}

async function load(src) {
  const t0 = performance.now();
  bsa = await openBsa(src);
  status(`${src.name.split(/[\\/]/).pop()}: v${bsa.version}, ${bsa.folderCount} folders, ${bsa.fileCount} files, `
    + `${(src.size / 2 ** 20).toFixed(0)} MiB, index read in ${(performance.now() - t0).toFixed(0)} ms`);
  $('filter').disabled = false;
  renderList();
  window.__bsaInfo = { version: bsa.version, folders: bsa.folderCount, files: bsa.fileCount, flags: bsa.flags };
  return bsa;
}

$('pick').onclick = async () => {
  try {
    const dir = await showDirectoryPicker({ id: 'fnv', mode: 'read' });
    let data = dir;
    try { data = await dir.getDirectoryHandle('Data'); } catch { /* picked Data itself */ }
    const fh = await data.getFileHandle(BSA_NAME);
    await load(fileSource(await fh.getFile()));
  } catch (e) { status(`Could not open ${BSA_NAME}: ${e.message}`); }
};
$('filter').oninput = renderList;
addEventListener('resize', draw);

// Dev-only automation hook: ?dev=1 reads the archive over HTTP range requests from the localhost dev server.
if (new URLSearchParams(location.search).has('dev')) {
  window.__dev = async () => {
    const url = `/__dev_data/${encodeURIComponent(BSA_NAME)}`;
    const size = +(await fetch(url, { method: 'HEAD' })).headers.get('content-length');
    await load(urlSource(url, size));
  };
  window.__show = (path) => show(bsa.files.find((f) => f.path === path), null);
  window.__files = () => shown.map((f) => f.path);
}
