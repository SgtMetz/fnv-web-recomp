// Debugging helpers for web/recomp/index.html, loaded on demand from the console or tools:
//   const d = await import('/debug.js'); await d.snap(); await d.keys([0xd0, 0xd0, 0x1c]);
// snap() posts the back buffer to the dev server (out/recomp/snap.png); keys() taps DirectInput scan codes.
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function snap(name) {
  const blob = await Promise.race([window.renderer.capture(), sleep(8000).then(() => null)]);
  if (!blob) return false;
  await fetch('/snap' + (name ? '?name=' + encodeURIComponent(name) : ''), { method: 'POST', body: blob });
  return true;
}

export async function keys(codes, gap = 400) {
  for (const c of codes) {
    window.Module._fnv_key(c, 1);
    await sleep(120);
    window.Module._fnv_key(c, 0);
    await sleep(gap);
  }
}

export async function mouse(dx, dy, steps = 10) {
  for (let i = 0; i < steps; i++) { window.Module._fnv_mouse(Math.round(dx / steps), Math.round(dy / steps), 0, -1, 0); await sleep(30); }
}

export async function click(button = 0) {
  window.Module._fnv_mouse(0, 0, 0, button, 1);
  await sleep(120);
  window.Module._fnv_mouse(0, 0, 0, button, 0);
}

export function status() {
  return document.getElementById('status').textContent;
}

// main menu -> New -> Yes (the menu takes about a minute to appear after the page loads)
export async function newGame(menuWaitMs = 65000) {
  await sleep(menuWaitMs);
  await keys([0xd0, 0xd0], 1200);
  await keys([0x1c], 2500);
  await keys([0x1c], 500);
}

// New Game, then dismiss the DLC item pop-ups
export async function intoGame(menuWaitMs = 65000) {
  await newGame(menuWaitMs);
  await sleep(20000);
  for (let i = 0; i < 6; i++) await keys([0x1c], 3500);
}
