// Prepended to the Emscripten build (tools/recomp_build_web.py). A wasm trap (memory out of bounds, unreachable)
// ends the node process with an uncaught exception; the 'exit' event still runs synchronously with the wasm instance
// in memory, so the guest state is reported there (fnv_dump_state writes to the --log file).
var Module = typeof Module !== 'undefined' ? Module : {};
// node: hand FNV_* environment variables to getenv() (Emscripten's environment starts from a fixed default set)
if (typeof process !== 'undefined' && process.env) {
  Module['preRun'] = (Module['preRun'] || []).concat([function () {
    for (var k in process.env) if (k.indexOf('FNV_') === 0) ENV[k] = process.env[k];
  }]);
}
if (typeof process !== 'undefined' && process.on && typeof require !== 'undefined' &&
    require('worker_threads').isMainThread) {
  process.on('exit', function (code) {
    if (code === 0 || !Module['_fnv_dump_state']) return;
    try {
      Module['_fnv_dump_state']();
    } catch (x) {
      console.error('fnv_dump_state failed: ' + x);
    }
  });
}
