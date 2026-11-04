// INI files as GetPrivateProfileString/WritePrivateProfileString see them: section and key names compare
// case-insensitively, the first matching key wins, whitespace around names and values is ignored, and comments,
// blank lines and order are kept on rewrite. Text is ANSI (latin1 here; the game's INIs are ASCII).

export function parseIni(text) {
  const lines = String(text).split(/\r?\n/);
  if (lines.length && lines[lines.length - 1] === '') lines.pop();
  return { lines };
}

const fold = (s) => s.trim().toLowerCase();

function sectionOf(line) {
  const m = /^\s*\[([^\]]*)\]/.exec(line);
  return m ? m[1] : null;
}

function keyOf(line) {
  if (/^\s*[;#]/.test(line)) return null;
  const i = line.indexOf('=');
  return i > 0 ? line.slice(0, i).trim() : null;
}

export function getIni(ini, section, key) {
  let cur = null;
  for (const line of ini.lines) {
    const s = sectionOf(line);
    if (s !== null) { cur = fold(s); continue; }
    if (cur === fold(section) && keyOf(line) !== null && fold(keyOf(line)) === fold(key))
      return line.slice(line.indexOf('=') + 1).trim();
  }
  return undefined;
}

export function setIni(ini, section, key, value) {
  let cur = null, sectionEnd = -1;
  for (let i = 0; i < ini.lines.length; i++) {
    const line = ini.lines[i];
    const s = sectionOf(line);
    if (s !== null) { cur = fold(s); continue; }
    if (cur !== fold(section)) continue;
    if (line.trim()) sectionEnd = i;
    const k = keyOf(line);
    if (k !== null && fold(k) === fold(key)) { ini.lines[i] = `${k}=${value}`; return; }
  }
  if (sectionEnd >= 0) { ini.lines.splice(sectionEnd + 1, 0, `${key}=${value}`); return; }
  const idx = ini.lines.findIndex((l) => sectionOf(l) !== null && fold(sectionOf(l)) === fold(section));
  if (idx >= 0) { ini.lines.splice(idx + 1, 0, `${key}=${value}`); return; }
  if (ini.lines.length && ini.lines[ini.lines.length - 1].trim()) ini.lines.push('');
  ini.lines.push(`[${section}]`, `${key}=${value}`);
}

export function serializeIni(ini) {
  return ini.lines.join('\r\n') + '\r\n';
}

export const latin1 = {
  decode: (bytes) => new TextDecoder('latin1').decode(bytes),
  encode: (text) => Uint8Array.from(String(text), (c) => { const x = c.charCodeAt(0); return x < 256 ? x : 0x3f; }),
};
