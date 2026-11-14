// ⚠️ BOUND BY OVERLAY-INVARIANTS.md (§7, §8, §11, §16). The two replaced functions are called by
// Archicad from anywhere, at any time: each decides from plain reads and returns. No ACAPI, no
// lock, no allocation, no window query.
// ArchViz/OverlayInputPointerShim -- where Archicad is told the pointer is, over a locked view.
//
// ⚠️ ARCHICAD DOES NOT WAIT TO BE TOLD WHERE THE POINTER IS: IT ASKS (the user, 2026-11-14, #145:
// "mouse hover still passes through the locked virtual editor and shows Archicad's native
// tooltips; the cursor icon keeps switching back and forth"). The locked view eats every hover
// move (114-264 eaten per 5 s in that run) and refuses WM_MOUSEHOVER, and Archicad still set its
// pre-selection cursors over it -- the arrow with a Mercedes, the Mercedes, the arrow with a
// chain, resources 310, 359 and 394 of Archicad.exe (HOSTCURSOR) -- up to 159 times in one drag.
// Those cursors mean it found an element's edge under the pointer, which it can only have by
// reading the pointer's position itself. Its dialog and control libraries do: DG.dll imports
// user32's GetCursorPos, UC.dll imports GetCursorPos and GetMessagePos, and Archicad.exe imports
// neither (the installed Archicad 29 binaries' import tables, read 2026-11-14).
//
// So those two libraries' import slots for the two functions are pointed at ours. While a locked
// view's HUD owns the pointer and Archicad is not navigating, they are told the pointer is
// nowhere on the desktop; otherwise they get the real answer from user32. Only those slots:
// nothing is detoured, user32 itself is untouched, and this add-on's own calls go to user32.
//
// ⚠️ THE REAL POSITION WHENEVER ARCHICAD NAVIGATES. It zooms about the pointer and orbits from
// it: while the middle button is held (read live), through the settle after any navigation
// event, and in a pointer mode of its own (Explore, Orbit, Pan, Zoom -- the HUD does not own the
// pointer then) the answer is user32's.
// ⚠️ ONLY ON THE THREAD THE INPUT LAYER RUNS ON. What it knows of the pointer is main-thread
// state; any other thread is given the real position.
//
// ⚠️ NOT SEEN LIVE (§7). That the element under the pointer is found through THESE imports, and
// not a third path, is the assumption. NAVWATCH therefore counts every question asked through
// them and how many were answered "nowhere": native tooltips with "asked +0" mean the position
// comes from somewhere else, and nothing here was the repair.

#include "ArchViz/OverlayInputEvents.hpp"
#include "ArchViz/OverlayInputState.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <atomic>
#include <cstring>
#include <string>

namespace geomsrv::archviz::overlayinput {

namespace state {

namespace {

// Where a pointer that is the HUD's is said to be: off every monitor, as a minimised window is.
constexpr LONG kNowhere = -32000;

DWORD s_thread = 0; // the input layer's thread: the shim's state is read on no other
std::atomic<uint32_t> s_asked { 0 };
std::atomic<uint32_t> s_told { 0 };

bool HidesNow ()
{
    if (s_thread == 0 || ::GetCurrentThreadId () != s_thread)
        return false;
    const bool middle = (::GetAsyncKeyState (VK_MBUTTON) & 0x8000) != 0;
    const uint64_t now = ::GetTickCount64 ();
    for (const Target& target : targets)
        if (HidesPointerFromHost (target.canvas != nullptr && target.map.locked && target.claims, middle,
                                  NavigationSettling (now, target.lastNavigationMs)))
            return true;
    return false;
}

BOOL WINAPI ShimGetCursorPos (LPPOINT point)
{
    s_asked.fetch_add (1, std::memory_order_relaxed);
    if (point != nullptr && HidesNow ()) {
        s_told.fetch_add (1, std::memory_order_relaxed);
        point->x = point->y = kNowhere;
        return TRUE;
    }
    return ::GetCursorPos (point);
}

DWORD WINAPI ShimGetMessagePos ()
{
    s_asked.fetch_add (1, std::memory_order_relaxed);
    if (HidesNow ()) {
        s_told.fetch_add (1, std::memory_order_relaxed);
        return DWORD (MAKELONG (kNowhere, kNowhere));
    }
    return ::GetMessagePos ();
}

struct Patch {
    const wchar_t* module;
    const char* function;
    void* shim;
    void** slot = nullptr;
    void* original = nullptr;
};
Patch s_patches[] = {
    { L"DG.dll", "GetCursorPos", reinterpret_cast<void*> (&ShimGetCursorPos) },
    { L"UC.dll", "GetCursorPos", reinterpret_cast<void*> (&ShimGetCursorPos) },
    { L"UC.dll", "GetMessagePos", reinterpret_cast<void*> (&ShimGetMessagePos) },
};

} // namespace

void** ImportSlot (HMODULE module, const char* library, const char* function)
{
    if (module == nullptr)
        return nullptr;
    auto* const base = reinterpret_cast<uint8_t*> (module);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*> (base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*> (base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0)
        return nullptr;
    for (const auto* entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*> (base + imports.VirtualAddress);
         entry->Name != 0; ++entry) {
        if (entry->OriginalFirstThunk == 0 || _stricmp (reinterpret_cast<const char*> (base + entry->Name), library))
            continue;
        const auto* name = reinterpret_cast<const IMAGE_THUNK_DATA*> (base + entry->OriginalFirstThunk);
        auto* slot = reinterpret_cast<IMAGE_THUNK_DATA*> (base + entry->FirstThunk);
        for (; name->u1.AddressOfData != 0; ++name, ++slot) {
            if (IMAGE_SNAP_BY_ORDINAL (name->u1.Ordinal))
                continue;
            const auto* const by = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*> (base + name->u1.AddressOfData);
            if (std::strcmp (by->Name, function) == 0)
                return reinterpret_cast<void**> (&slot->u1.Function);
        }
    }
    return nullptr;
}

// ⚠️ COMPARE-AND-SWAP, AS THE CONTEXT HOOK'S SLOTS (§11): a plain store can put ours over a pointer
// someone wrote a moment later, or put an old original back over a newer one.
bool SwapImport (void** slot, void* expected, void* replacement)
{
    if (slot == nullptr)
        return false;
    DWORD previous = 0;
    if (!::VirtualProtect (slot, sizeof (void*), PAGE_READWRITE, &previous))
        return false;
    const bool swapped = ::InterlockedCompareExchangePointer (slot, replacement, expected) == expected;
    DWORD ignored = 0;
    ::VirtualProtect (slot, sizeof (void*), previous, &ignored);
    return swapped;
}

void InstallPointerShim ()
{
    s_thread = ::GetCurrentThreadId ();
    s_asked = s_told = 0;
    std::string said;
    for (Patch& patch : s_patches) {
        if (patch.slot != nullptr)
            continue; // already ours
        char name[64] = {};
        ::WideCharToMultiByte (CP_UTF8, 0, patch.module, -1, name, sizeof (name), nullptr, nullptr);
        void** const slot = ImportSlot (::GetModuleHandleW (patch.module), "USER32.dll", patch.function);
        void* const original = slot != nullptr ? *slot : nullptr;
        if (slot != nullptr && original != patch.shim && SwapImport (slot, original, patch.shim)) {
            patch.slot = slot;
            patch.original = original;
            said += std::string (said.empty () ? "" : ", ") + name + "!" + patch.function;
        }
        else
            said += std::string (said.empty () ? "" : ", ") + name + "!" + patch.function +
                    (slot == nullptr ? " NOT FOUND" : " NOT SWAPPED");
    }
    ArchVizLog ("POINTERSHIM  where Archicad asks for the pointer, over a locked view it is told nowhere: " + said);
}

void RemovePointerShim ()
{
    bool any = false;
    for (Patch& patch : s_patches) {
        if (patch.slot == nullptr)
            continue;
        any = true;
        // The module may have been unloaded with its slots; ours is put back only where it still is.
        if (::GetModuleHandleW (patch.module) != nullptr && !SwapImport (patch.slot, patch.shim, patch.original))
            ArchVizLog (std::string ("POINTERSHIM  ") + patch.function +
                        ": the slot no longer held ours at removal; left as found");
        patch.slot = nullptr;
        patch.original = nullptr;
    }
    for (Target& target : targets)
        target.claims = false;
    if (any)
        ArchVizLog ("POINTERSHIM  removed: Archicad is told the pointer's position again");
    s_thread = 0;
}

void NotePointerClaims ()
{
    for (Target& target : targets)
        target.claims = target.canvas != nullptr && target.map.locked && OwnsPointer (target);
}

} // namespace state

void TakePointerShimCounts (uint32_t& asked, uint32_t& told)
{
    asked = state::s_asked.exchange (0, std::memory_order_relaxed);
    told = state::s_told.exchange (0, std::memory_order_relaxed);
}

} // namespace geomsrv::archviz::overlayinput
