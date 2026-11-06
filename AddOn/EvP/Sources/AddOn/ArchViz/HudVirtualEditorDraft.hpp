#ifndef TAPIOCA_HUD_VIRTUAL_EDITOR_DRAFT_HPP
#define TAPIOCA_HUD_VIRTUAL_EDITOR_DRAFT_HPP

// Durable virtual-editor drafts (#159 D1): explicit Save / Open of the whole authored
// landscape to a versioned file (Edit/EditDraft.hpp), unsaved-work tracking, and the
// Save / Discard / Cancel prompt before a user close. No native model writes.
//
// Storage policy (D1): one folder, %LOCALAPPDATA%\Tapioca\Drafts, one `<name>.tapdraft`
// per draft the user names. Saving replaces that file atomically (temporary file, then a
// rename); a failed write leaves the old file and the unsaved state as they were. Opening
// reads into a validated candidate and applies it as one undoable edit, so the work it
// replaced is one Undo away; a malformed or newer-format file changes nothing. Undo
// history is not saved. Recovery after a crash (D2) is not part of this slice.
//
// File IO is requested from layout and done by FollowDraftFiles on the message loop,
// never in ImGui layout or Present, in the debug export's pattern.
#include <filesystem>
#include <string>

namespace geomsrv::archviz::edithud {
struct Session;

// Whether the authored content differs from the last durable save or open (or from the
// session's start, before either). Cached per document revision.
bool DraftDirty (Session& session);
// Done / Esc / Close: closes at once with nothing unsaved; with unsaved landscape work it
// asks first (Session::closePrompt).
void RequestClose (Session& session);
// Requests; false (with the reason in `message`) while input is unfinished or the name is
// unusable. A Save from the prompt closes the editor once the file is written.
bool RequestDraftSave (Session& session, bool closeAfter = false);
bool RequestDraftOpen (Session& session, const std::string& file);
bool RequestDraftList (Session& session);
// The message-loop side. The directory overload keeps tests away from the user's drafts.
bool FollowDraftFiles (Session& session, const std::filesystem::path& directory);
bool FollowDraftFiles (Session& session);
// The panel's draft row and the unsaved-work prompt (layout only; they request, never write).
void DraftControls (Session& session);
void ClosePrompt (Session& session);
} // namespace geomsrv::archviz::edithud
#endif
