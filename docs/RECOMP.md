# Whole-game recompilation (Phase 3 path, chosen 2026-11-07)

Author: Claude Opus 5.5. The user chose to build the browser version by lifting the whole original executable,
rather than compiling decomp C++ to WebAssembly. clang has no Microsoft C++ ABI on wasm, so decomp objects could not
share vtables and layouts with the game's data. The decomp stays the reference for names,
understanding and debugging.

## Model

- **One CPU state per guest thread** (`FnvCpu`: eight registers, flags, x87 stack, XMM, MMX, `fs` base), passed to
  every function. Each original function `0xXXXXXXXX` becomes `void f_XXXXXXXX(FnvCpu& s)`.
- **Real guest stack.** Each guest thread has a stack in guest memory, and `esp` points into it. `call` pushes the
  original return address (the next instruction's address) and calls the target's host function. `ret N` does
  `esp += 4 + N` and returns to the host caller. Because the return address is on the stack, code that reads it
  works: `__SEH_prolog4`, `_chkstk` / `_alloca_probe`, `_EH_prolog`, `call $+5; pop`. Helpers that leave `esp`
  unbalanced also work, because callers continue from the shared `esp`.
- **Guest memory = host memory at the original addresses.** The exe's sections are mapped at their virtual
  addresses (0x00400000…0x0140c9e4), and heaps and stacks are allocated in the same 32-bit space. `M32(a)` is a direct
  load. On native x86 this is the process address space (the host program is based above 0x10000000); in wasm32 it
  is linear memory starting at address 0.
- **Direct calls** to a known function compile to a direct C++ call. **Indirect calls and jumps** (vtables,
  callbacks, `jmp eax`) go through `fnv_dispatch(s, addr)`: a table from original address to host function, built
  from every lifted entry. A jump into the middle of another function is lifted as an entry of its own.
- **Tail jumps** call the target without pushing; the target's `ret` returns for both.
- **Imports.** At start-up each IAT slot gets an address in a reserved *host range*. The dispatcher sends those to
  host implementations (`h_CreateFileA(s)` reads its arguments at `esp+4`, sets `eax`, pops like the real stdcall
  function). COM objects the host hands out (Direct3D 9, DirectInput, DirectSound) are guest-memory objects whose
  vtables hold host-range addresses.
- **Callbacks** from the host into guest code (window procedure, thread start, timers, enumeration callbacks) go
  through `fnv_guest_call(s, addr, args...)`: push the arguments and a sentinel return address, then dispatch.
- **`fs:`** is a per-thread TEB block in guest memory: `[0]` SEH head, `[4]`/`[8]` stack bounds, `[0x18]` self,
  `[0x2c]` TLS array. The original static CRT keeps working on it, including its own TLS (`__tls_index`).
- **Threads.** Native: real OS threads, each with its own `FnvCpu` and guest stack. Browser: pthreads on
  SharedArrayBuffer (needs COOP/COEP headers), or one-at-a-time coroutines, as Phase 2's thread host planned.
- **x87** stays the current double-based emulation. It now lives in the shared state, so float results cross
  calls without the old `fpush` plumbing.
- **SEH / C++ exceptions** (later milestone). `RaiseException` (host) walks the guest SEH chain and calls the
  original handlers (`__CxxFrameHandler3`, lifted). `_JumpToContinuation` resumes at a catch continuation: functions
  with try blocks get a host `setjmp` landing pad keyed by their guest frame, and the continuation becomes a
  `longjmp` plus a `switch` to the label. Until this exists an exception stops the run with a report.

## Milestones

1. **Native full-lift build.** Every function is lifted, and no decomp code is compiled. A small host program maps
   the player's exe, installs host imports that call real Win32 directly (guest pointers are host pointers here),
   and runs the original entry point `mainCRTStartup`, so the original CRT start-up runs too. Exit: main menu.
2. **Emscripten build of the same code, headless in node.** Host imports are the browser `pal_win32` (files through
   the OPFS layer in `web/`). Exit: load `FalloutNV.esm`, `coc GSDocMitchellHouse`, tick for
   60 s, exit cleanly.
3. **Browser.** The game in a worker (PROXY_TO_PTHREAD), files from the local dev server (`server/serve.mjs`,
   `pal_fs_web.cpp`), the D3D9 model sending a command ring that `web/recomp/renderer.js` draws with WebGPU and the
   translated shaders, input through `fnv_key`/`fnv_mouse`, audio still to do. Exit: the main menu in a browser
   (reached 2026-11-07, `out/recomp/browser_main_menu.png`), then the opening.

## Performance notes (to measure, not assumed)

The lifter computes flags eagerly. Each body keeps the registers and flags it uses in locals (`FnvLocal`, written
back at calls, returns and traps), so clang drops the dead flag work and keeps registers out of memory. Memory accesses through a
distinct struct type let clang's type-based alias analysis keep `FnvCpu` fields in registers across guest stores.
