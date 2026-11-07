// Guest COM objects for the PAL (Direct3D 9, DirectInput, DirectSound). An object lives in guest memory as
// [vtable][host object pointer][reference count]; its vtable holds host-function addresses built from the SDK method
// tables (out/recomp/com_tables.inc, tools/gen_com_tables.py). Methods without an implementation log their first call
// and return E_NOTIMPL, popping the right number of bytes; AddRef/Release/QueryInterface have defaults.
#pragma once
#include "pal.h"

#include <initializer_list>
#include <utility>

struct ComMethod { const char* name; uint32_t arg_bytes; };
struct ComInterface { const char* name; const ComMethod* methods; uint32_t count; };

struct ComClass;
// The interface's class, with these methods implemented (the rest default). Built once per interface name.
ComClass* com_class(const char* iface, std::initializer_list<std::pair<const char*, HostFn>> impl);
// A new guest object of that class around `host` (reference count 1).
uint32_t com_new(ComClass* cls, void* host);
inline void* com_host(uint32_t obj) { return (void*)(uintptr_t)M32(obj + 4); }
inline uint32_t com_refs(uint32_t obj) { return M32(obj + 8); }

// HRESULTs
static const uint32_t S_OK_ = 0, E_NOTIMPL_ = 0x80004001u, E_NOINTERFACE_ = 0x80004002u, E_FAIL_ = 0x80004005u,
                      E_OUTOFMEMORY_ = 0x8007000Eu;
