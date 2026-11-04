// Windows path semantics on top of OPFS, which is case-sensitive and has no drive letters or backslashes.
//
// The game builds paths with mixed case ("%s%s.NAM" for DEADMONEY.NAM), mixes '\' and '/', and lists directories
// with FindFirstFileA wildcards whose result order feeds its plugin load order (TESDataHandler 0x004624b0 sorts
// the listing by ftLastWriteTime, and ties keep the listing order). These helpers reproduce what NTFS gives it.

// Split a path into components: '\' and '/' both separate, empty and '.' parts are dropped, '..' pops.
export function splitPath(path) {
  const out = [];
  for (const part of String(path).split(/[\\/]+/)) {
    if (!part || part === '.') continue;
    if (part === '..') { out.pop(); continue; }
    out.push(part);
  }
  return out;
}

export function joinPath(...parts) {
  return splitPath(parts.filter((p) => p !== undefined && p !== '').join('/')).join('/');
}

// Lookup key: NTFS compares names after upcasing each UTF-16 unit (the volume's $UpCase table; for the names the
// game uses this is plain toUpperCase on each unit). Windows also ignores trailing dots and spaces in a component.
export function foldName(name) {
  let s = String(name).replace(/[. ]+$/, '');
  let out = '';
  for (const ch of s) {
    const u = ch.toUpperCase();
    out += u.length === ch.length ? u : ch;  // one-to-one mappings only, as $UpCase ('ß' stays 'ß')
  }
  return out;
}

export function foldPath(path) {
  return splitPath(path).map(foldName).join('/');
}

// Directory listing order on NTFS: binary order of the upcased UTF-16 units.
export function compareNtfs(a, b) {
  const x = foldName(a), y = foldName(b);
  const n = Math.min(x.length, y.length);
  for (let i = 0; i < n; i++) {
    const d = x.charCodeAt(i) - y.charCodeAt(i);
    if (d) return d;
  }
  return x.length - y.length;
}

// FindFirstFile wildcard match: '*' any run, '?' one character, case-insensitive. "*.*" and "*" match everything,
// including names without a dot. (The 8.3 short-name quirks of real Windows are not reproduced.)
export function wildcardMatch(pattern, name) {
  const p = foldName(pattern), s = foldName(name);
  if (p === '*' || p === '*.*') return true;
  let pi = 0, si = 0, star = -1, mark = 0;
  while (si < s.length) {
    if (pi < p.length && (p[pi] === '?' || p[pi] === s[si])) { pi++; si++; }
    else if (pi < p.length && p[pi] === '*') { star = pi++; mark = si; }
    else if (star >= 0) { pi = star + 1; si = ++mark; }
    else return false;
  }
  while (pi < p.length && (p[pi] === '*' || (p[pi] === '.' && pi === p.length - 1))) pi++;
  return pi === p.length;
}

// Windows FILETIME (100 ns ticks since 1601-01-01 UTC) <-> JS milliseconds since 1970.
const EPOCH_DIFF_MS = 11644473600000;
export function msToFiletime(ms) {
  return (BigInt(Math.round(ms)) + BigInt(EPOCH_DIFF_MS)) * 10000n;
}
export function filetimeToMs(ft) {
  return Number(BigInt(ft) / 10000n - BigInt(EPOCH_DIFF_MS));
}
