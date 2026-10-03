#ifndef EVP_ARCHVIZ_HUDCONSOLE_HPP
#define EVP_ARCHVIZ_HUDCONSOLE_HPP

// ArchViz/HudConsole -- what the HUDs' Debug tab says went wrong, and the few things the user
// checks it against.
//
// ⚠️ NOT A LOG (the user, 2026-10-03: a console on the Debug tab that shows the key things when
// an error happens -- not a wordy log, the very important things the user checks when something
// is failing). archviz.log keeps every line; this keeps what a user can act on: a surface that
// did not start, a write that was refused, a model that could not be read -- and, as notes, the
// handful of moments to read them against (the overlay on or off, the viewer opened). Said in
// words, with where it came from and when; the same thing said again in a row is one entry with
// its count. The last `kKept` are kept, the process over: a project closing takes nothing.
//
// ⚠️ ANY THREAD -- BUT NEVER INSIDE THE OVERLAY'S PRESENT HOOK, which takes no lock and allocates
// nothing (OVERLAY-INVARIANTS.md §11): this does both. Say it where the failure is noticed on
// the main, render or extraction thread.
//
// The HUDs draw it (`Draw`): the overlays' engine from what its owner hands it (OverlayHud.hpp
// `OwnPages::console`), the viewer's from `Entries` on its render thread. Each HUD keeps its own
// marks -- what it has shown, what its Clear hid -- so clearing one clears no other.
//
// Pure apart from ImGui: tests/cpp builds it.

#include "ArchViz/OverlayLayers.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace hudconsole {

enum class Level : uint8_t { Note = 0, Warning = 1, Error = 2 };

struct Entry {
    uint64_t sequence = 0; // one more for every entry said, a repeat's too: the newest's is the largest
    Level level = Level::Note;
    std::string source; // where it came from: "Overlay", "Viewer", "Metadata", "Model"
    std::string text;
    std::string time; // when it was said last, local "14:03:27"
    uint32_t repeats = 1;
};

constexpr size_t kKept = 64;

// Say it. The same level, source and text as the newest entry: that entry, once more, with this
// time and a new sequence. A new entry wakes the listener (`SetListener`) after; a repeat does not.
void Say (Level level, const std::string& source, const std::string& text);
inline void Error (const std::string& source, const std::string& text)
{
    Say (Level::Error, source, text);
}
inline void Warning (const std::string& source, const std::string& text)
{
    Say (Level::Warning, source, text);
}
inline void Note (const std::string& source, const std::string& text)
{
    Say (Level::Note, source, text);
}

// What is kept, oldest first.
std::vector<Entry> Entries ();

// Called after every entry, outside the console's lock, on the sayer's thread: the overlays'
// owner wakes their HUDs with it (a new entry is laid out without waiting for the pointer).
void SetListener (std::function<void ()> listener);

// Forget every entry (tests; the add-on keeps them for the process).
void Clear ();

// ---- on a HUD's Debug tab ---------------------------------------------------------------------

// What the Debug tab's title counts: the errors and warnings newer than both `seen` and
// `cleared`.
uint32_t Unseen (const std::vector<Entry>& entries, uint64_t seen, uint64_t cleared);

// The console, on a page: newest first, each entry newer than `cleared` -- its level as a mark of
// its colour, its time and source muted, its words wrapped, "x3" for a repeat -- under a row with
// Copy (what is shown, to the clipboard) and Clear (moves `cleared` to the newest: this HUD shows
// nothing older). `seen` moves to the newest shown. Nothing kept: a line saying so.
void Draw (const std::vector<Entry>& entries, const overlaylayers::Panel& look, uint64_t& seen, uint64_t& cleared);

} // namespace hudconsole
} // namespace archviz
} // namespace geomsrv

#endif
