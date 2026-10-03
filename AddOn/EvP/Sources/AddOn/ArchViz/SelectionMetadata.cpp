// ArchViz/SelectionMetadata -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/SelectionMetadata.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/TextPrompt.hpp"
#include "Metadata/MetadataStorage.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <utility>

namespace geomsrv {
namespace archviz {
namespace selectionmetadata {

namespace {

namespace meta = metadata;

constexpr UINT kEditMessage = WM_APP + 0x52;
constexpr UINT kReadMessage = WM_APP + 0x53;
constexpr UINT kLaterMessage = WM_APP + 0x54;
constexpr wchar_t kClassName[] = L"TapiocaSelectionMetadata";

struct Pending {
    std::vector<hudmeta::Edit> edits;
    std::vector<std::string> guids;
    std::function<void ()> done;
};

std::atomic<HWND> g_window { nullptr };
bool g_classRegistered = false;
std::mutex g_mutex; // the queue and the last error
std::deque<Pending> g_queue;
std::string g_lastError;
// The one element a render thread's HUD asked for (PageOf): its page once read, by GUID.
struct Slot {
    std::string asked;
    std::string held;
    hudmeta::Page page;
};
Slot g_slot;
std::deque<std::function<void ()>> g_later;
// ⚠️ A DIALOG RUNS ITS OWN MESSAGE LOOP, which delivers the next request's message while the
// first is still asking: that one is left queued, and the write in progress drains it after.
bool g_writing = false;
std::atomic<uint64_t> g_requested { 0 }, g_dropped { 0 }, g_steps { 0 }, g_written { 0 }, g_refused { 0 };

HINSTANCE Module ()
{
    HMODULE module = nullptr;
    ::GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR> (&Module), &module);
    return module;
}

int64_t NowMs ()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now ().time_since_epoch ())
        .count ();
}

void Fail (const std::string& error)
{
    ArchVizLog ("METADATA     " + error);
    std::lock_guard<std::mutex> lock (g_mutex);
    g_lastError = error;
}

// What the user typed for every edit that asks; false when they cancelled -- the request goes.
bool AskTexts (std::vector<hudmeta::Edit>& edits)
{
    for (hudmeta::Edit& edit : edits) {
        if (edit.action != hudmeta::Edit::Action::AskText)
            continue;
        std::string answer;
        if (!textprompt::Ask (edit.label.empty () ? edit.id : edit.label, edit.text, answer))
            return false;
        edit.action = hudmeta::Edit::Action::Set;
        edit.text = answer;
    }
    return true;
}

// The undo step's name in the Edit menu: "Undo Tapioca: Usage".
GS::UniString StepName (const std::vector<hudmeta::Edit>& edits)
{
    const std::string what =
        edits.size () == 1 && !edits.front ().label.empty () ? edits.front ().label : std::string ("metadata");
    return GS::UniString (("Tapioca: " + what).c_str (), CC_UTF8);
}

void Write (Pending& pending)
{
    // Asked before the undo step opens: a modal dialog inside one would run inside the step.
    if (!AskTexts (pending.edits))
        return;
    const std::vector<std::string> guids = pending.guids.empty () ? SelectedGuids () : pending.guids;
    if (guids.empty ()) {
        Fail ("nothing is selected to write the metadata to");
        return;
    }
    meta::ProjectSchema schema;
    bool stored = false;
    std::string error;
    if (!meta::storage::ReadSchema (schema, stored, error)) {
        Fail (error);
        return;
    }
    const int64_t now = NowMs ();
    uint64_t written = 0, refused = 0;
    std::string first;
    const auto note = [&] (const std::string& why) {
        if (first.empty ())
            first = why;
    };
    const GSErrCode err = ACAPI_CallUndoableCommand (StepName (pending.edits), [&] () -> GSErrCode {
        for (const std::string& guid : guids) {
            meta::EntityMetadata entity;
            bool present = false;
            std::string why;
            if (!meta::storage::Read (guid, entity, present, why)) {
                ++refused;
                note (why);
                continue;
            }
            // ⚠️ WRITTEN ONLY WHEN CHANGED: a tag taken off a mixed selection would otherwise
            // leave empty metadata on every element that never had it.
            const std::string before = meta::ToJson (entity);
            for (const hudmeta::Edit& edit : pending.edits)
                if ((edit.element.empty () || edit.element == guid) && !hudmeta::Apply (entity, edit, schema, now, why))
                    note (why);
            if (meta::ToJson (entity) == before)
                continue;
            // ⚠️ THE SCHEMA FIRST: an element it refuses keeps what it had.
            const std::vector<std::string> problems = meta::Validate (entity, schema);
            if (!problems.empty ()) {
                ++refused;
                note (guid + ": " + problems.front ());
                continue;
            }
            if (!meta::storage::Write (guid, std::move (entity), why)) {
                ++refused;
                note (why);
                continue;
            }
            ++written;
        }
        return NoError;
    });
    g_steps.fetch_add (1, std::memory_order_relaxed);
    g_written.fetch_add (written, std::memory_order_relaxed);
    g_refused.fetch_add (refused, std::memory_order_relaxed);
    if (err != NoError)
        Fail ("the undo step was refused (error " + std::to_string (int (err)) + ")");
    else if (!first.empty ())
        Fail (first);
    else
        ArchVizLog ("METADATA     " + std::to_string (pending.edits.size ()) + " edit(s) written to " +
                    std::to_string (written) + " element(s)");
}

// The element asked for, read -- on the main thread.
void ReadAsked ()
{
    std::string guid;
    {
        std::lock_guard<std::mutex> lock (g_mutex);
        guid = g_slot.asked;
    }
    if (guid.empty ())
        return;
    hudmeta::Page page = Read ({ guid }, 1);
    std::lock_guard<std::mutex> lock (g_mutex);
    if (g_slot.asked != guid)
        return; // another was asked for meanwhile: its own message reads it
    g_slot.held = guid;
    g_slot.page = std::move (page);
}

void Drain ()
{
    if (g_writing)
        return;
    g_writing = true;
    for (;;) {
        Pending pending;
        {
            std::lock_guard<std::mutex> lock (g_mutex);
            if (g_queue.empty ())
                break;
            pending = std::move (g_queue.front ());
            g_queue.pop_front ();
        }
        Write (pending);
        if (pending.done)
            pending.done ();
    }
    g_writing = false;
    // What was written may be the element a render thread's HUD shows.
    ReadAsked ();
}

LRESULT CALLBACK WindowProc (HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kEditMessage) {
        Drain ();
        return 0;
    }
    if (message == kReadMessage) {
        ReadAsked ();
        return 0;
    }
    if (message == kLaterMessage) {
        std::deque<std::function<void ()>> work;
        {
            std::lock_guard<std::mutex> lock (g_mutex);
            work.swap (g_later);
        }
        for (const std::function<void ()>& job : work)
            job ();
        return 0;
    }
    return ::DefWindowProcW (window, message, wParam, lParam);
}

} // namespace

std::vector<std::string> SelectedGuids ()
{
    std::vector<std::string> out;
    API_SelectionInfo info = {};
    GS::Array<API_Neig> neigs;
    const GSErrCode err = ACAPI_Selection_Get (&info, &neigs, false);
    // The marquee's handle is ours to free, selected or not (SelectionBridge.cpp says why).
    if (info.marquee.coords != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*> (&info.marquee.coords));
    if (err != NoError)
        return out; // APIERR_NOSEL among others: nothing selected
    for (UInt32 i = 0; i < neigs.GetSize (); ++i)
        out.push_back (APIGuidToString (neigs[i].guid).ToCStr ().Get ());
    return out;
}

hudmeta::Page Read (const std::vector<std::string>& listed, uint32_t selected)
{
    meta::ProjectSchema schema;
    bool stored = false;
    std::string error;
    if (!meta::storage::ReadSchema (schema, stored, error)) {
        hudmeta::Page page;
        page.known = true;
        page.selected = selected;
        page.note = error;
        return page;
    }
    std::vector<meta::EntityMetadata> entities;
    std::string first;
    for (const std::string& guid : listed) {
        meta::EntityMetadata entity;
        bool present = false;
        if (meta::storage::Read (guid, entity, present, error))
            entities.push_back (std::move (entity));
        else if (first.empty ())
            first = error;
    }
    hudmeta::Page page = hudmeta::Fields (schema, entities, selected);
    page.note = first;
    return page;
}

void Request (std::vector<hudmeta::Edit> edits, std::vector<std::string> guids, std::function<void ()> done)
{
    if (edits.empty ())
        return;
    g_requested.fetch_add (1, std::memory_order_relaxed);
    const HWND window = g_window.load (std::memory_order_acquire);
    if (window == nullptr) {
        g_dropped.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    {
        std::lock_guard<std::mutex> lock (g_mutex);
        g_queue.push_back ({ std::move (edits), std::move (guids), std::move (done) });
    }
    // Not posted, it waits for the next request's message, which drains every one queued.
    if (!::PostMessageW (window, kEditMessage, 0, 0))
        g_dropped.fetch_add (1, std::memory_order_relaxed);
}

hudmeta::Page PageOf (const std::string& guid)
{
    if (guid.empty ())
        return hudmeta::Page {};
    {
        std::lock_guard<std::mutex> lock (g_mutex);
        if (g_slot.held == guid && g_slot.asked == guid)
            return g_slot.page;
        if (g_slot.asked == guid)
            return hudmeta::Page {}; // asked, not read yet
        g_slot.asked = guid;
    }
    const HWND window = g_window.load (std::memory_order_acquire);
    if (window != nullptr)
        ::PostMessageW (window, kReadMessage, 0, 0);
    return hudmeta::Page {};
}

void Later (std::function<void ()> work)
{
    const HWND window = g_window.load (std::memory_order_acquire);
    if (window == nullptr || !work)
        return;
    {
        std::lock_guard<std::mutex> lock (g_mutex);
        g_later.push_back (std::move (work));
    }
    ::PostMessageW (window, kLaterMessage, 0, 0);
}

void Arm ()
{
    if (g_window.load (std::memory_order_acquire) != nullptr)
        return;
    const HINSTANCE instance = Module ();
    if (!g_classRegistered) {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof (windowClass);
        windowClass.lpfnWndProc = &WindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kClassName;
        g_classRegistered = ::RegisterClassExW (&windowClass) != 0 || ::GetLastError () == ERROR_CLASS_ALREADY_EXISTS;
    }
    const HWND window = g_classRegistered ? ::CreateWindowExW (0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                                               instance, nullptr)
                                          : nullptr;
    if (window == nullptr)
        ArchVizLog ("METADATA     the editor's window could not be made: the HUD's metadata edits will not be written");
    g_window.store (window, std::memory_order_release);
}

void Shutdown ()
{
    const HWND window = g_window.exchange (nullptr, std::memory_order_acq_rel);
    if (window != nullptr)
        ::DestroyWindow (window);
    if (g_classRegistered) {
        ::UnregisterClassW (kClassName, Module ());
        g_classRegistered = false;
    }
    std::lock_guard<std::mutex> lock (g_mutex);
    g_queue.clear ();
    g_slot = Slot {};
    g_later.clear ();
}

Stats GetStats ()
{
    Stats stats;
    stats.requested = g_requested.load (std::memory_order_relaxed);
    stats.dropped = g_dropped.load (std::memory_order_relaxed);
    stats.steps = g_steps.load (std::memory_order_relaxed);
    stats.written = g_written.load (std::memory_order_relaxed);
    stats.refused = g_refused.load (std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock (g_mutex);
    stats.lastError = g_lastError;
    return stats;
}

} // namespace selectionmetadata
} // namespace archviz
} // namespace geomsrv
