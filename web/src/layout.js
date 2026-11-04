// Where things live in the origin-private file system (OPFS). The browser platform layer maps the game's Windows
// paths onto these folders, the same way the native loader gives the game a private "My Games" copy:
//
//   <root>/game/                 install folder (the game's working directory): Fallout_default.ini, presets, Data/
//   <root>/docs/                 CSIDL_PERSONAL: My Games/FalloutNV/{Fallout.ini, FalloutPrefs.ini, Saves/}
//   <root>/localappdata/         CSIDL_LOCAL_APPDATA: FalloutNV/plugins.txt
//   <root>/meta/import.json      what was imported: groups, sizes, and each file's original last-write time
//   <root>/meta/times.json       last-write times of files the game wrote (OPFS cannot set a file's time)
//   <root>/cache/                derived data made on this machine (translated shaders, transcoded video)
//
// <root> is "fnv" for the real runtime. Tests use their own root so they never touch an import.

export const ROOT = 'fnv';
export const GAME = 'game';
export const DOCS = 'docs';
export const LOCALAPPDATA = 'localappdata';
export const META = 'meta';
export const CACHE = 'cache';

export const MY_GAMES = 'docs/My Games/FalloutNV';
export const SAVES = 'docs/My Games/FalloutNV/Saves';
export const PLUGINS_TXT = 'localappdata/FalloutNV/plugins.txt';
export const IMPORT_META = 'meta/import.json';
export const TIMES_META = 'meta/times.json';

// Windows path prefixes the platform layer presents to the game, and the OPFS folder each one maps to.
// The game itself only sees these strings (from GetModuleFileName, GetCurrentDirectory and SHGetFolderPath).
export const MOUNTS = [
  { win: 'C:\\Games\\FalloutNV', dir: GAME },
  { win: 'C:\\Users\\Player\\Documents', dir: DOCS },
  { win: 'C:\\Users\\Player\\AppData\\Local', dir: LOCALAPPDATA },
];
export const CWD = MOUNTS[0].win;
