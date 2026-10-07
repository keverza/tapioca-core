// ArchViz/HudConsole -- see the header.

#include "ArchViz/HudConsole.hpp"

#include "ArchViz/HudShell.hpp"

#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow: the edge a page wraps its text at

#include <algorithm>
#include <cmath>
#include <ctime>
#include <deque>
#include <mutex>
#include <limits>
#include <utility>

namespace geomsrv {
namespace archviz {
namespace hudconsole {

namespace {

// Function-local: said from any thread, at any time after the DLL loaded.
std::mutex& Mutex ()
{
    static std::mutex mutex;
    return mutex;
}

std::deque<Entry>& Store ()
{
    static std::deque<Entry> store;
    return store;
}

std::function<void ()>& Listener ()
{
    static std::function<void ()> listener;
    return listener;
}

uint64_t g_sequence = 0; // under Mutex ()

std::string Now ()
{
    const std::time_t now = std::time (nullptr);
    std::tm local = {};
#ifdef _WIN32
    localtime_s (&local, &now);
#else
    localtime_r (&now, &local);
#endif
    char text[16] = {};
    std::strftime (text, sizeof (text), "%H:%M:%S", &local);
    return text;
}

const char* Word (Level level)
{
    switch (level) {
        case Level::Error:
            return "ERROR";
        case Level::Warning:
            return "WARNING";
        case Level::Note:
            return "NOTE";
    }
    return "NOTE";
}

uint32_t Colour (Level level, const overlaylayers::Panel& look)
{
    switch (level) {
        case Level::Error:
            return hudshell::kErrorRgba;
        case Level::Warning:
            return hudshell::kBusyRgba;
        case Level::Note:
            return hudshell::WithAlpha (look.textRgba, 0.45f);
    }
    return look.textRgba;
}

} // namespace

void Say (Level level, const std::string& source, const std::string& text)
{
    std::function<void ()> listener;
    {
        std::lock_guard<std::mutex> lock (Mutex ());
        std::deque<Entry>& store = Store ();
        const std::string time = Now ();
        if (!store.empty () && store.back ().level == level && store.back ().source == source &&
            store.back ().text == text) {
            Entry& last = store.back ();
            if (last.repeats < (std::numeric_limits<uint32_t>::max) ())
                ++last.repeats;
            last.time = time;
            last.sequence = ++g_sequence;
        }
        else {
            Entry entry;
            entry.sequence = ++g_sequence;
            entry.level = level;
            entry.source = source;
            entry.text = text;
            entry.time = time;
            store.push_back (std::move (entry));
            while (store.size () > kKept)
                store.pop_front ();
            // ⚠️ ONLY A NEW ENTRY WAKES ANYONE: a failure noticed while a HUD lays out is said
            // again at every layout, and a repeat that woke the HUD would lay it out for ever.
            listener = Listener ();
        }
    }
    if (listener)
        listener ();
}

std::vector<Entry> Entries ()
{
    std::lock_guard<std::mutex> lock (Mutex ());
    return std::vector<Entry> (Store ().begin (), Store ().end ());
}

void SetListener (std::function<void ()> listener)
{
    std::lock_guard<std::mutex> lock (Mutex ());
    Listener () = std::move (listener);
}

void Clear ()
{
    std::lock_guard<std::mutex> lock (Mutex ());
    Store ().clear ();
}

uint32_t Unseen (const std::vector<Entry>& entries, uint64_t seen, uint64_t cleared)
{
    uint32_t unseen = 0;
    for (const Entry& entry : entries)
        if (entry.level != Level::Note && entry.sequence > seen && entry.sequence > cleared)
            ++unseen;
    return unseen;
}

bool Draw (const std::vector<Entry>& entries, const overlaylayers::Panel& look, uint64_t& seen, uint64_t& cleared)
{
    // Newest first: what went wrong last is what the user came to read.
    std::vector<const Entry*> shown;
    for (auto entry = entries.rbegin (); entry != entries.rend () && shown.size () < kKept; ++entry)
        if (entry->sequence > cleared)
            shown.push_back (&*entry);
    uint64_t newest = 0;
    for (const Entry& entry : entries)
        newest = (std::max) (newest, entry.sequence);

    ImGui::SeparatorText ("Console");
    ImGui::BeginDisabled (shown.empty ());
    if (ImGui::SmallButton ("Copy##console")) {
        std::string all;
        for (const Entry* entry : shown)
            all += entry->time + "  " + Word (entry->level) + "  " + entry->source + "  " + entry->text +
                   (entry->repeats > 1 ? "  (x" + std::to_string (entry->repeats) + ")" : std::string ()) + "\n";
        ImGui::SetClipboardText (all.c_str ());
    }
    hudshell::Tip ("Copy what the console shows, to paste where it is needed", hudshell::TipSide::Above);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Clear##console"))
        cleared = newest;
    hudshell::Tip ("Hide what is shown here; what comes next is shown", hudshell::TipSide::Above);
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    const bool openLogs = ImGui::SmallButton ("Open logs##console");
    hudshell::Tip ("Open %LOCALAPPDATA%\\Tapioca\\logs in Explorer", hudshell::TipSide::Above);
    if (shown.empty ()) {
        ImGui::TextDisabled ("Nothing has gone wrong");
        return openLogs;
    }
    const float em = ImGui::GetFontSize ();
    const float indent = std::floor (0.9f * em);
    for (size_t k = 0; k < shown.size (); ++k) {
        const Entry& entry = *shown[k];
        ImGui::PushID (int (k));
        // Its level: a disc of its colour, level with the first line.
        const ImVec2 at = ImGui::GetCursorScreenPos ();
        const float radius = std::floor (0.28f * em) + 0.5f;
        ImGui::GetWindowDrawList ()->AddCircleFilled (ImVec2 (at.x + radius, at.y + std::floor (0.55f * em)), radius,
                                                      hudshell::Packed (Colour (entry.level, look)));
        ImGui::Indent (indent);
        ImGui::PushStyleColor (ImGuiCol_Text, hudshell::Colour (hudshell::WithAlpha (look.textRgba, 0.6f)));
        const std::string head = entry.time + "  " + entry.source +
                                 (entry.repeats > 1 ? "  x" + std::to_string (entry.repeats) : std::string ());
        ImGui::TextUnformatted (head.c_str ());
        ImGui::PopStyleColor ();
        // Its words, wrapped: at the page's edge in a panel of a set width, at a reading width
        // otherwise -- a long message never widens the panel.
        float wrap = ImGui::GetCursorPosX () + 20.0f * em;
        if (const float edge = ImGui::GetCurrentWindow ()->DC.TextWrapPos; edge > 0.0f)
            wrap = (std::min) (wrap, edge);
        ImGui::PushTextWrapPos (wrap);
        ImGui::TextUnformatted (entry.text.c_str ());
        ImGui::PopTextWrapPos ();
        ImGui::Unindent (indent);
        ImGui::PopID ();
    }
    seen = (std::max) (seen, newest);
    return openLogs;
}

} // namespace hudconsole
} // namespace archviz
} // namespace geomsrv
