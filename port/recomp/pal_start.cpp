// Start mode (FNV_START=quick): begin a new game outside Doc Mitchell's house with the default character, as if the
// player had taken every default in the opening.
//
// It works through the game's own console path. MenuConsole's "run a line" branch (0x0071c7fc in ConsoleManager::Idle)
// builds a Script on the stack, sets its text and compiles and runs it with the console's ScriptCompiler. This file
// makes the same calls from the main thread, from the keyboard poll (pal_input.cpp), outside rendering:
//   1. Once the main menu has been up for a moment (the start menu exists and the player is in no cell),
//      `coc GSDocMitchellHouse`. From the main menu that starts a fresh game in Doc's house, as New Game does.
//   2. Once the player has been in the world for a moment, the script lines the opening (quest VCG01, FalloutNV.esm)
//      would have run by the time the player walks out of the door:
//      - stage 200 and VGenericTimer event 3: VCG01 done, VMQ01/VCG04 started, VCG02 at stage 5, 8 AM;
//      - default tag skills: VCG01TestSCRIPT's picks with no quiz answers, Guns, Speech and Sneak;
//      - Doc's farewell dialogue:
//        - INFO 001057e4: the Pip-Boy, equipped. The new player already carries it equipped (checked), so only
//          ResetPipBoyManager runs; removing and re-adding the equipped Pip-Boy hung the main thread;
//        - INFO 001057e8: the Courier's note, 4 stimpaks, 18 caps, 6 lockpicks, and a 9mm pistol with 60 rounds
//          (the Guns-tagged branch);
//        - INFO 001057e7: the front door made usable, controls restored, casual mode (the hardcore prompt's
//          default);
//      - the opening's player package and image-space effects removed.
//   3. The player goes out the front door: `player.moveto` the exterior door (GSDocMitchellHouseIntDoorREF's teleport
//      target, 00103e69), which loads the exterior. Once the player is in the new cell, setpos/setangle put them
//      exactly where the door's teleport (XTEL) leads, on Doc's porch.
// The defaults: the name Courier, a male with the default face, SPECIAL 5 each, and no traits.
//
// Base forms and quests are named by form ID: the runtime keeps editor IDs only for references, so the console
// answers "Item 'VCG01' not found" for names (checked in this build). Form IDs come from FalloutNV.esm.
#include "pal.h"

#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <string>

namespace {
// addresses in FalloutNV.exe 1.4.0.525 (names from the decomp's @fnv annotations)
const uint32_t kPlayer = 0x011dea3c;            // PlayerCharacter* g_thePlayer
const uint32_t kStartMenu = 0x011daac0;         // StartMenu* (exists while the main or pause menu is up)
const uint32_t kRefParentCell = 0x40;           // TESObjectREFR::parentCell
const uint32_t kConsoleInstance = 0x0071b160;   // ConsoleManager::Instance(bool create), cdecl
const uint32_t kScriptCtor = 0x005aa0f0;        // Script::Script()                (thiscall)
const uint32_t kScriptDtor = 0x005aa1a0;        // Script::~Script()
const uint32_t kFormChanged = 0x00484490;       // TESForm::NotifySaveLoadGameOfFormChange()
const uint32_t kScriptSetText = 0x005abe50;     // Script::SetText(const char*)
const uint32_t kScriptCompile = 0x005ac400;     // Script::CompileScriptText(ScriptCompiler*, COMPILER_NAME, TESObjectREFR*)
const uint32_t kCompilerSystemWindow = 1;       // the console's compiler name

const char* const kStartCell = "coc GSDocMitchellHouse";
const char* const kAfterDocsHouse[] = {
    "SetPlayerTagSkill Guns 0",
    "SetPlayerTagSkill Speech 1",
    "SetPlayerTagSkill Sneak 2",
    "SetStage 00104C1C 200",                          // VCG01
    "SetStage 0010A214 5",                            // VCG02
    "player.removescriptpackage",
    "RemoveImageSpaceModifier 00120E95",              // VCG01FadeInFromBlackISFX
    "RemoveImageSpaceModifier 00120E96",              // VCG01PlayerStrainISFX
    "EnablePlayerControls 1 1 1 1 1 1 1",
    "GSDocMitchellHouseIntDoorREF.SetDestroyed 0",
    "ResetPipBoyManager",
    "AddNote 00105BCD",                               // VMQ01CourierNote
    "player.additem 00015169 4 1",                    // Stimpak
    "player.additem 0000000F 18 1",                   // Caps001
    "player.additem 0000000A 6 1",                    // Lockpick
    "player.additem 000E3982 1 1",                    // CondNV9mmPistolLoot
    "player.additem 00096BDF 60 1",                   // Ammo9mm75
    "SetHardcore 0",
    "player.moveto 00103E69",
};
const char* const kOnPorch[] = {
    "player.setpos x -73162.9",
    "player.setpos y 1312.4",
    "player.setpos z 8686.4",
    "player.setangle z 72.8",
};

enum State { kOff, kWaitMenu, kWaitWorld, kWaitOutside, kDone };
State g_state = kOff;
uint32_t g_house_cell;
std::chrono::steady_clock::time_point g_since;
bool g_timing;

// true once `cond` has held for `ms` without a break
bool held(bool cond, int ms)
{
    auto now = std::chrono::steady_clock::now();
    if (!cond) { g_timing = false; return false; }
    if (!g_timing) { g_timing = true; g_since = now; }
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - g_since).count() >= ms;
}

uint32_t this_call(FnvCpu& s, uint32_t fn, uint32_t self, const uint32_t* args, int n)
{
    s.ecx = self;
    return fnv_guest_call(s, fn, args, n);
}

// one console line, run the way the console runs it
void run_line(FnvCpu& s, const char* line)
{
    uint32_t a1 = 1;
    uint32_t console = fnv_guest_call(s, kConsoleInstance, &a1, 1);
    if (!console || !M32(console)) { fnv_host_log("[start] no console\n"); return; }
    uint32_t compiler = M32(console);                                  // ConsoleManager::m_pScriptCompiler
    char* text = (char*)calloc(1, strlen(line) + 1);
    strcpy(text, line);
    uint32_t script = (uint32_t)(uintptr_t)calloc(1, 0x200);           // Script (0x54 bytes) with room to spare
    this_call(s, kScriptCtor, script, nullptr, 0);
    this_call(s, kFormChanged, script, nullptr, 0);
    uint32_t t = (uint32_t)(uintptr_t)text;
    this_call(s, kScriptSetText, script, &t, 1);
    uint32_t c[3] = {compiler, kCompilerSystemWindow, 0};               // no selected reference
    this_call(s, kScriptCompile, script, c, 3);
    this_call(s, kScriptDtor, script, nullptr, 0);
    free((void*)(uintptr_t)script);
    free(text);
    fnv_host_log((std::string("[start] ") + line + "\n").c_str());
}
} // namespace

std::mutex g_queue_mx;
std::deque<std::string> g_queue;     // console lines from the host (pal_console_line), run at the next poll

void pal_start_quick() { g_state = kWaitMenu; }
void pal_console_line(const char* line)
{
    std::lock_guard<std::mutex> lk(g_queue_mx);
    g_queue.push_back(line);
}

// called from the keyboard's GetDeviceState: the main thread, once per input poll
void pal_start_tick(FnvCpu& s)
{
    for (;;)
    {
        std::string line;
        {
            std::lock_guard<std::mutex> lk(g_queue_mx);
            if (g_queue.empty()) break;
            line = g_queue.front();
            g_queue.pop_front();
        }
        uint32_t eax = s.eax, ecx = s.ecx, edx = s.edx;
        run_line(s, line.c_str());
        s.eax = eax; s.ecx = ecx; s.edx = edx;
    }
    if (g_state == kOff || g_state == kDone) return;
    uint32_t player = M32(kPlayer);
    uint32_t cell = player ? M32(player + kRefParentCell) : 0;
    if (g_state == kWaitMenu)
    {
        if (held(M32(kStartMenu) && player && !cell, 2500))
        {
            uint32_t eax = s.eax, ecx = s.ecx, edx = s.edx;
            run_line(s, kStartCell);
            s.eax = eax; s.ecx = ecx; s.edx = edx;
            g_state = kWaitWorld;
            g_timing = false;
        }
    }
    else if (g_state == kWaitWorld)
    {
        if (held(cell != 0, 3000))
        {
            uint32_t eax = s.eax, ecx = s.ecx, edx = s.edx;
            g_house_cell = cell;
            for (const char* line : kAfterDocsHouse) run_line(s, line);
            s.eax = eax; s.ecx = ecx; s.edx = edx;
            g_state = kWaitOutside;
            g_timing = false;
        }
    }
    else if (g_state == kWaitOutside)
    {
        if (held(cell && cell != g_house_cell, 1500))
        {
            uint32_t eax = s.eax, ecx = s.ecx, edx = s.edx;
            for (const char* line : kOnPorch) run_line(s, line);
            s.eax = eax; s.ecx = ecx; s.edx = edx;
            g_state = kDone;
        }
    }
}
