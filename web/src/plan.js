// Match an install listing against the manifest and decide what to copy.
//
// Rules: every file of a required group must be present, or the import cannot start. A DLC group is offered only
// when all of its files are present (its plugin needs its archives). Other optional groups copy whatever members
// exist. A size that differs from the reference install is a warning, not an error (other editions and languages
// ship other builds of some files). Files no group covers (mods, tools, the executables) are counted, not copied.
import { foldPath } from './winpath.js';

export function planImport(manifest, entries, choices = {}) {
  const byKey = new Map();
  const warnings = [], errors = [], info = [];
  for (const e of entries) {
    const key = foldPath(e.path);
    if (byKey.has(key)) { warnings.push(`two files differ only in case: ${byKey.get(key).path} and ${e.path}; using the first`); continue; }
    byKey.set(key, e);
  }
  const claimed = new Set();
  const groups = [];
  for (const g of manifest.groups) {
    const files = [], missing = [], mismatched = [];
    for (const m of g.members) {
      if (m.path) {
        const key = foldPath(m.path);
        const e = byKey.get(key);
        if (!e) { missing.push(m.path); continue; }
        claimed.add(key);
        files.push(e);
        if (e.size !== m.size) mismatched.push({ path: e.path, size: e.size, expected: m.size });
      } else {
        const prefix = foldPath(m.dir) + '/';
        const under = [...byKey].filter(([k]) => k.startsWith(prefix)).map(([k, e]) => (claimed.add(k), e));
        if (!under.length) { missing.push(m.dir); continue; }
        files.push(...under);
        const bytes = under.reduce((s, e) => s + e.size, 0);
        if (under.length !== m.files || bytes !== m.bytes)
          mismatched.push({ path: m.dir, size: bytes, expected: m.bytes, files: under.length, expectedFiles: m.files });
      }
    }
    const complete = missing.length === 0;
    const available = g.required || g.dlc ? complete : files.length > 0;
    const wanted = choices[g.id] ?? g.selected;
    const selected = g.required || (available && !!wanted);
    if (g.required && !complete) errors.push(`${g.label}: missing ${missing.join(', ')}`);
    else if (g.dlc && !complete && files.length) warnings.push(`${g.label} is incomplete (missing ${missing.join(', ')}), so it cannot be imported`);
    else if (!available) info.push(`${g.label}: not installed`);
    for (const mm of mismatched) {
      warnings.push(mm.files !== undefined
        ? `${mm.path}: ${mm.files} files, ${mm.size} bytes (reference install: ${mm.expectedFiles} files, ${mm.expected} bytes)`
        : `${mm.path}: ${mm.size} bytes (reference install: ${mm.expected})`);
    }
    groups.push({
      id: g.id, label: g.label, required: g.required, dlc: !!g.dlc, plugin: g.plugin, available, selected,
      missing, mismatched, files, bytes: files.reduce((s, e) => s + e.size, 0),
    });
  }
  const copy = groups.filter((g) => g.selected).flatMap((g) => g.files);
  const unclaimed = [...byKey].filter(([k]) => !claimed.has(k)).map(([, e]) => e);
  if (unclaimed.length) info.push(`${unclaimed.length} other files are not imported (executables, tools, mods)`);
  return {
    groups, copy, errors, warnings, info, unclaimed: unclaimed.length,
    totalBytes: copy.reduce((s, e) => s + e.size, 0),
    plugins: groups.filter((g) => g.selected && g.plugin).map((g) => g.plugin),
  };
}
