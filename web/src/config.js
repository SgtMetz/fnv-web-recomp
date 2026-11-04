// First-run configuration, matching what the PC launcher leaves behind on a fresh install:
//   docs/My Games/FalloutNV/Fallout.ini       byte copy of the install's Fallout_default.ini
//   docs/My Games/FalloutNV/FalloutPrefs.ini  the chosen quality preset (low/medium/high/VeryHigh.ini) plus
//                                             browser display settings; the game rewrites this file itself
//   localappdata/FalloutNV/plugins.txt        empty, as in the reference install: the DLC load through their
//                                             .NAM files (TESDataHandler 0x004624b0), and a "lite" import just
//                                             leaves those files out
// Evidence for the preset step: the reference machine's FalloutPrefs.ini matches 36 of VeryHigh.ini's 41 keys
// (high 17, medium 10, low 2), and its Fallout.ini is byte-identical to Fallout_default.ini.
//
// Existing files are kept unless `overwrite` is set, so the player's settings survive a re-import.
import { dirAt, readBytes, writeBytes, exists } from './opfs.js';
import { parseIni, setIni, serializeIni, latin1 } from './ini.js';
import { MY_GAMES, PLUGINS_TXT, GAME } from './layout.js';
import { foldName } from './winpath.js';

export const PRESETS = ['low', 'medium', 'high', 'VeryHigh'];

// Browser display overrides: windowed into the canvas at its size; the page handles fullscreen.
export function displayOverrides({ width, height }) {
  return { Display: { 'bFull Screen': '0', 'iSize W': String(width), 'iSize H': String(height) } };
}

async function findInGame(root, name) {
  const game = await root.getDirectoryHandle(GAME);
  for await (const [n, h] of game.entries()) if (h.kind === 'file' && foldName(n) === foldName(name)) return `${GAME}/${n}`;
  return null;
}

export async function seedConfig(root, { preset = 'high', width = 1280, height = 720, overwrite = false } = {}) {
  const done = [];
  const iniPath = `${MY_GAMES}/Fallout.ini`, prefsPath = `${MY_GAMES}/FalloutPrefs.ini`;
  if (overwrite || !(await exists(root, iniPath))) {
    const src = await findInGame(root, 'Fallout_default.ini');
    if (!src) throw new Error('Fallout_default.ini was not imported');
    await writeBytes(root, iniPath, await readBytes(root, src));
    done.push(iniPath);
  }
  if (overwrite || !(await exists(root, prefsPath))) {
    const src = await findInGame(root, `${preset}.ini`);
    const ini = parseIni(src ? latin1.decode(await readBytes(root, src)) : '');
    for (const [section, keys] of Object.entries(displayOverrides({ width, height })))
      for (const [k, v] of Object.entries(keys)) setIni(ini, section, k, v);
    await writeBytes(root, prefsPath, latin1.encode(serializeIni(ini)));
    done.push(src ? prefsPath : `${prefsPath} (no ${preset}.ini in the install: display settings only)`);
  }
  if (overwrite || !(await exists(root, PLUGINS_TXT))) {
    await writeBytes(root, PLUGINS_TXT, new Uint8Array(0));
    done.push(PLUGINS_TXT);
  }
  await dirAt(root, `${MY_GAMES}/Saves`, true);
  return done;
}
