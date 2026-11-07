// Prepended to the Emscripten build (tools/recomp_build_web.py). A wasm trap (memory out of bounds, unreachable)
// ends the node process with an uncaught exception; the 'exit' event still runs synchronously with the wasm instance
// in memory, so the guest state is reported there (fnv_dump_state writes to the --log file).
var Module = typeof Module !== 'undefined' ? Module : {};
// hand FNV_* settings to getenv() (Emscripten's environment starts from a fixed default set): node's environment, or
// in the browser the page's Module.fnvEnv (web/recomp/index.html, from the query string)
Module['preRun'] = (Module['preRun'] || []).concat([function () {
  if (typeof process !== 'undefined' && process.env && process.versions && process.versions.node) {
    for (var k in process.env) if (k.indexOf('FNV_') === 0) ENV[k] = process.env[k];
  }
  if (Module['fnvEnv']) for (var k2 in Module['fnvEnv']) ENV[k2] = Module['fnvEnv'][k2];
}]);
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
