// PAL DirectShow: the filter graph the game's music player builds around its own decoder filter (RunAudioChannel
// 0x830b30). The graph accepts the filter, runs on a clock and reports position, duration and completion, so the music
// logic advances tracks as on Windows; nothing is decoded or played yet. CoCreateInstance(CLSID_FilterGraph) creates it.
#include "pal_com.h"

#include <string.h>

#include <chrono>
#include <mutex>

namespace {
#define A(i) harg(s, (i))
#define RET(v, n) return hret(s, (uint32_t)(v), (n))
const double kTrackSeconds = 120.0;          // nominal length (the decoder filter is not asked)

double now_s()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Graph
{
    uint32_t filter = 0;
    int state = 0;                           // 0 stopped, 1 paused, 2 running
    double pos = 0, started = 0;             // position at the last state change, and when it started running
    bool completed = false, reported = false;
    int32_t volume = 0, balance = 0;
    uint32_t obj[6] = {};                    // filter graph/builder, control, audio, position, event, seeking
    std::mutex mx;
    double position()
    {
        double p = state == 2 ? pos + now_s() - started : pos;
        if (p >= kTrackSeconds) { p = kTrackSeconds; completed = true; }
        return p;
    }
};
Graph* G(FnvCpu& s) { return (Graph*)com_host(A(0)); }

void qi(FnvCpu& s)
{
    Graph* g = G(s);
    uint32_t d1 = M32(A(1)), out = A(2);
    int i = d1 == 0x56a868a9u || d1 == 0x56a8689fu || d1 == 0 ? 0 : d1 == 0x56a868b1u ? 1 : d1 == 0x56a868b3u ? 2 :
            d1 == 0x56a868b2u ? 3 : (d1 == 0x56a868b6u || d1 == 0x56a868c0u) ? 4 : d1 == 0x36b73880u ? 5 : -1;
    if (i < 0 || !g->obj[i]) { M32(out) = 0; RET(0x80004002u, 3); }
    M32(out) = g->obj[i];
    ++M32(g->obj[i] + 8);
    RET(0, 3);
}
void ok(FnvCpu& s, int n) { hret(s, 0, n); }
void ok1(FnvCpu& s) { ok(s, 1); }
void ok2(FnvCpu& s) { ok(s, 2); }
void ok3(FnvCpu& s) { ok(s, 3); }
void ok4(FnvCpu& s) { ok(s, 4); }
void ok6(FnvCpu& s) { ok(s, 6); }
void ok9(FnvCpu& s) { ok(s, 9); }
void notimpl(FnvCpu& s, int n) { hret(s, 0x80004001u, n); }

// IFilterGraph / IGraphBuilder
void gb_AddFilter(FnvCpu& s) { Graph* g = G(s); g->filter = A(1); if (A(1)) ++M32(A(1) + 8); RET(0, 3); }

// IMediaControl
void mc_set(FnvCpu& s, int st)
{
    Graph* g = G(s);
    std::lock_guard<std::mutex> lk(g->mx);
    g->pos = g->position();
    if (st == 0) g->pos = g->pos >= kTrackSeconds ? 0 : g->pos;
    g->state = st;
    g->started = now_s();
}
void mc_Run(FnvCpu& s) { mc_set(s, 2); RET(0, 1); }
void mc_Pause(FnvCpu& s) { mc_set(s, 1); RET(0, 1); }
void mc_Stop(FnvCpu& s) { mc_set(s, 0); RET(0, 1); }
void mc_GetState(FnvCpu& s) { M32(A(2)) = (uint32_t)G(s)->state; RET(0, 3); }

// IBasicAudio
void ba_put_Volume(FnvCpu& s) { G(s)->volume = (int32_t)A(1); RET(0, 2); }
void ba_get_Volume(FnvCpu& s) { M32(A(1)) = (uint32_t)G(s)->volume; RET(0, 2); }
void ba_put_Balance(FnvCpu& s) { G(s)->balance = (int32_t)A(1); RET(0, 2); }
void ba_get_Balance(FnvCpu& s) { M32(A(1)) = (uint32_t)G(s)->balance; RET(0, 2); }

// IMediaPosition (REFTIME = double seconds; by-value doubles take two stack words)
void put_double(uint32_t p, double v) { memcpy((void*)(uintptr_t)p, &v, 8); }
double arg_double(FnvCpu& s, int i) { double v; uint32_t w[2] = {A(i), A(i + 1)}; memcpy(&v, w, 8); return v; }
void mp_get_Duration(FnvCpu& s) { put_double(A(1), kTrackSeconds); RET(0, 2); }
void mp_put_CurrentPosition(FnvCpu& s)
{
    Graph* g = G(s);
    std::lock_guard<std::mutex> lk(g->mx);
    g->pos = arg_double(s, 1);
    g->started = now_s();
    g->completed = g->reported = false;
    RET(0, 3);
}
void mp_get_CurrentPosition(FnvCpu& s) { Graph* g = G(s); std::lock_guard<std::mutex> lk(g->mx); put_double(A(1), g->position()); RET(0, 2); }
void mp_get_double_zero(FnvCpu& s) { put_double(A(1), 0); RET(0, 2); }
void mp_get_Rate(FnvCpu& s) { put_double(A(1), 1.0); RET(0, 2); }
void mp_put_double(FnvCpu& s) { RET(0, 3); }
void mp_Can(FnvCpu& s) { M32(A(1)) = 0xffffffffu; RET(0, 2); }       // OATRUE

// IMediaEvent(Ex)
void me_GetEventHandle(FnvCpu& s) { M32(A(1)) = 0; RET(0, 2); }
void me_GetEvent(FnvCpu& s)
{
    Graph* g = G(s);
    std::lock_guard<std::mutex> lk(g->mx);
    g->position();
    if (g->completed && !g->reported)
    {
        g->reported = true;
        M32(A(1)) = 1;                                                // EC_COMPLETE
        M32(A(2)) = 0;
        M32(A(3)) = 0;
        RET(0, 5);
    }
    RET(0x80004004u, 5);                                              // E_ABORT: no event
}
void me_WaitForCompletion(FnvCpu& s) { M32(A(2)) = 0; RET(0x80040013u, 3); }

// IDispatch's four methods, shared by the dispatch-based interfaces
ComCustom disp(const char* n, uint32_t b) { return ComCustom{n, b, [](FnvCpu& s) { hret(s, 0x80004001u, 0); }}; }

ComClass* graph_class(int i)
{
    static ComClass* cls[6];
    if (cls[i]) return cls[i];
    HostFn release = nullptr;
    switch (i)
    {
    case 0:
        cls[0] = com_class_custom("IGraphBuilder", {
            {"QueryInterface", 12, qi}, {"AddRef", 4, nullptr}, {"Release", 4, release}, {"AddFilter", 12, gb_AddFilter},
            {"RemoveFilter", 8, ok2}, {"EnumFilters", 8, [](FnvCpu& s) { notimpl(s, 2); }},
            {"FindFilterByName", 12, [](FnvCpu& s) { M32(A(2)) = 0; hret(s, 0x80040216u, 3); }},
            {"ConnectDirect", 16, ok4}, {"Reconnect", 8, ok2}, {"Disconnect", 8, ok2}, {"SetDefaultSyncSource", 4, ok1},
            {"Connect", 12, ok3}, {"Render", 8, ok2}, {"RenderFile", 12, ok3}, {"AddSourceFilter", 16, ok4},
            {"SetLogFile", 8, ok2}, {"Abort", 4, ok1}, {"ShouldOperationContinue", 4, ok1}});
        break;
    case 1:
        cls[1] = com_class_custom("IMediaControl", {
            {"QueryInterface", 12, qi}, {"AddRef", 4, nullptr}, {"Release", 4, nullptr},
            {"GetTypeInfoCount", 8, nullptr}, {"GetTypeInfo", 16, nullptr}, {"GetIDsOfNames", 24, nullptr}, {"Invoke", 36, nullptr},
            {"Run", 4, mc_Run}, {"Pause", 4, mc_Pause}, {"Stop", 4, mc_Stop}, {"GetState", 12, mc_GetState},
            {"RenderFile", 8, ok2}, {"AddSourceFilter", 12, ok3}, {"get_FilterCollection", 8, ok2},
            {"get_RegFilterCollection", 8, ok2}, {"StopWhenReady", 4, mc_Stop}});
        break;
    case 2:
        cls[2] = com_class_custom("IBasicAudio", {
            {"QueryInterface", 12, qi}, {"AddRef", 4, nullptr}, {"Release", 4, nullptr},
            {"GetTypeInfoCount", 8, nullptr}, {"GetTypeInfo", 16, nullptr}, {"GetIDsOfNames", 24, nullptr}, {"Invoke", 36, nullptr},
            {"put_Volume", 8, ba_put_Volume}, {"get_Volume", 8, ba_get_Volume}, {"put_Balance", 8, ba_put_Balance},
            {"get_Balance", 8, ba_get_Balance}});
        break;
    case 3:
        cls[3] = com_class_custom("IMediaPosition", {
            {"QueryInterface", 12, qi}, {"AddRef", 4, nullptr}, {"Release", 4, nullptr},
            {"GetTypeInfoCount", 8, nullptr}, {"GetTypeInfo", 16, nullptr}, {"GetIDsOfNames", 24, nullptr}, {"Invoke", 36, nullptr},
            {"get_Duration", 8, mp_get_Duration}, {"put_CurrentPosition", 12, mp_put_CurrentPosition},
            {"get_CurrentPosition", 8, mp_get_CurrentPosition}, {"get_StopTime", 8, mp_get_Duration},
            {"put_StopTime", 12, mp_put_double}, {"get_PrerollTime", 8, mp_get_double_zero},
            {"put_PrerollTime", 12, mp_put_double}, {"put_Rate", 12, mp_put_double}, {"get_Rate", 8, mp_get_Rate},
            {"CanSeekForward", 8, mp_Can}, {"CanSeekBackward", 8, mp_Can}});
        break;
    case 4:
        cls[4] = com_class_custom("IMediaEventEx", {
            {"QueryInterface", 12, qi}, {"AddRef", 4, nullptr}, {"Release", 4, nullptr},
            {"GetTypeInfoCount", 8, nullptr}, {"GetTypeInfo", 16, nullptr}, {"GetIDsOfNames", 24, nullptr}, {"Invoke", 36, nullptr},
            {"GetEventHandle", 8, me_GetEventHandle}, {"GetEvent", 20, me_GetEvent},
            {"WaitForCompletion", 12, me_WaitForCompletion}, {"CancelDefaultHandling", 8, ok2},
            {"RestoreDefaultHandling", 8, ok2}, {"FreeEventParams", 16, ok4}, {"SetNotifyWindow", 16, ok4},
            {"SetNotifyFlags", 8, ok2}, {"GetNotifyFlags", 8, [](FnvCpu& s) { M32(A(1)) = 0; hret(s, 0, 2); }}});
        break;
    default:
        cls[5] = com_class_custom("IMediaSeeking", {{"QueryInterface", 12, qi}, {"AddRef", 4, nullptr}, {"Release", 4, nullptr}});
        break;
    }
    (void)ok6; (void)ok9;
    return cls[i];
}
} // namespace

// CoCreateInstance(CLSID_FilterGraph, ...): the graph's IFilterGraph interface; 0 for other classes
uint32_t pal_dshow_create(uint32_t clsid)
{
    if (M32(clsid) != 0xe436ebb3u) return 0;
    Graph* g = new Graph;
    for (int i = 0; i < 6; ++i) g->obj[i] = com_new(graph_class(i), g);
    return g->obj[0];
}
