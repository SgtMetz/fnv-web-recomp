// PAL COM layer: see pal_com.h.
#include "pal_com.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "../../out/recomp/com_tables.inc"

struct ComClass
{
    const ComInterface* iface;
    uint32_t vtbl;                  // guest address of the vtable
};

namespace {
struct StubInfo { const ComInterface* iface; uint32_t method; };
StubInfo g_stub_info[kHostFnMax];
std::atomic<bool> g_stub_seen[kHostFnMax];
std::mutex g_mx;
std::map<std::string, ComClass*> g_classes;

void default_method(FnvCpu& s)
{
    StubInfo& si = g_stub_info[fnv_host_current()];
    const ComMethod& m = si.iface->methods[si.method];
    uint32_t self = harg(s, 0);
    uint32_t r = E_NOTIMPL_;
    if (!strcmp(m.name, "AddRef"))
        r = ++M32(self + 8);
    else if (!strcmp(m.name, "Release"))
        r = M32(self + 8) ? --M32(self + 8) : 0;     // the object stays: guest memory is not reused here
    else if (!strcmp(m.name, "QueryInterface"))
    {                                                // one implementation per object: answer with itself
        M32(harg(s, 2)) = self;
        ++M32(self + 8);
        r = S_OK_;
    }
    else if (!g_stub_seen[fnv_host_current()].exchange(true))
    {
        char buf[160];
        snprintf(buf, sizeof buf, "[com] %s::%s not implemented (E_NOTIMPL)%c", si.iface->name, m.name, 10);
        fnv_host_log(buf);
    }
    s.eax = r;
    s.esp += 4 + m.arg_bytes;
}
} // namespace

ComClass* com_class(const char* iface, std::initializer_list<std::pair<const char*, HostFn>> impl)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_classes.find(iface);
    if (it != g_classes.end()) return it->second;
    const ComInterface* ci = nullptr;
    for (const ComInterface& c : kComInterfaces)
        if (!strcmp(c.name, iface)) ci = &c;
    if (!ci)
    {
        fnv_host_log("[com] unknown interface requested\n");
        abort();
    }
    ComClass* cls = new ComClass{ci, 0};
    uint32_t* vt = (uint32_t*)calloc(ci->count, 4);
    for (uint32_t i = 0; i < ci->count; ++i)
    {
        HostFn fn = nullptr;
        for (auto& p : impl)
            if (!strcmp(p.first, ci->methods[i].name)) fn = p.second;
        std::string label = std::string(iface) + "::" + ci->methods[i].name;
        uint32_t a = fnv_host_register(strdup(label.c_str()), fn ? fn : default_method);
        g_stub_info[(a - fnv_host_base()) / 16] = {ci, i};
        vt[i] = a;
    }
    for (auto& p : impl)
    {
        bool found = false;
        for (uint32_t i = 0; i < ci->count; ++i) found |= !strcmp(p.first, ci->methods[i].name);
        if (!found)
        {
            char buf[160];
            snprintf(buf, sizeof buf, "[com] %s has no method %s%c", iface, p.first, 10);
            fnv_host_log(buf);
        }
    }
    cls->vtbl = (uint32_t)(uintptr_t)vt;
    g_classes[iface] = cls;
    return cls;
}

uint32_t com_new(ComClass* cls, void* host)
{
    uint32_t* o = (uint32_t*)calloc(4, 4);
    o[0] = cls->vtbl;
    o[1] = (uint32_t)(uintptr_t)host;
    o[2] = 1;
    return (uint32_t)(uintptr_t)o;
}
