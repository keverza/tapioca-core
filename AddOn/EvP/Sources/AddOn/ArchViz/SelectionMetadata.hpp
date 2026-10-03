#ifndef EVP_ARCHVIZ_SELECTIONMETADATA_HPP
#define EVP_ARCHVIZ_SELECTIONMETADATA_HPP

// ArchViz/SelectionMetadata -- the HUDs' Selection page's Tapioca metadata, read from and
// written to the project (Metadata/MetadataStorage.hpp): the page for the elements the HUD
// lists, and the user's edits (HudMetadata.hpp) written to every element they name.
//
// ⚠️ AN EDIT IS WRITTEN FROM THE MESSAGE LOOP, NEVER FROM THE LAYOUT THAT TOOK IT: a HUD lays
// out inside ImGui's lock -- the viewer's on its render thread -- and an undo step opened
// there would run Archicad's notifications, and its redraws, inside the frame. `Request` is
// any thread's: it queues and posts to a message-only window; the main thread asks for any
// text first (a native dialog: TextPrompt.hpp), then writes every edit of the request to every
// element in ONE undo step, each element read, laid over, validated against the project's
// schema and written. An element the schema refuses is left as it was and said.
//
// MAIN THREAD: Read, Arm, Shutdown, SelectedGuids. Request, PageOf and GetStats: any thread.

#include "ArchViz/HudMetadata.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace selectionmetadata {

// Archicad's selection, as GUID strings.
std::vector<std::string> SelectedGuids ();

// The page for `listed` -- the elements the HUD reads -- of `selected` selected: the project's
// schema, their metadata, their fields. A failure is the page's note.
hudmeta::Page Read (const std::vector<std::string>& listed, uint32_t selected);

// `edits` for the elements `guids` -- Archicad's selection when empty, as it stands when they
// are written -- then `done` on the main thread, when given: the owner reads its page again.
void Request (std::vector<hudmeta::Edit> edits, std::vector<std::string> guids = {},
              std::function<void ()> done = nullptr);

// ⚠️ ONE ELEMENT'S PAGE FOR A HUD THAT CANNOT READ THE PROJECT -- the viewer's, on its render
// thread (the user, 2026-10-03: its Selection tab edits the picked element's scheme too). What
// was read for `guid` when it is the element asked for last; otherwise a page not yet `known`,
// and it is read on the main thread. Read again after every write that names it. Empty
// `guid`: nothing picked, nothing read.
hudmeta::Page PageOf (const std::string& guid);

// The message-only window the requests are posted to: made at load, gone at unload.
void Arm ();
void Shutdown ();

struct Stats {
    uint64_t requested = 0;
    uint64_t dropped = 0; // no window to post to
    uint64_t steps = 0;   // undo steps taken
    uint64_t written = 0; // elements written
    uint64_t refused = 0; // elements the schema or Archicad refused
    std::string lastError;
};
Stats GetStats ();

} // namespace selectionmetadata
} // namespace archviz
} // namespace geomsrv

#endif
