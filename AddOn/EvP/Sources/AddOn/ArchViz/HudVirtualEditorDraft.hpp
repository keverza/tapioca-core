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
// history is not saved.
//
// File IO is requested from layout and done by FollowDraftFiles on the message loop,
// never in ImGui layout or Present, in the debug export's pattern.
//
// Recovery (D2): %LOCALAPPDATA%\Tapioca\Drafts\Recovery, one `<id>.tapdraft` per editor
// session, `id` random per session. While there is unsaved work, a snapshot of the last
// COMMITTED revision replaces it atomically once input has been quiet for kRecoveryQuiet and
// at most every kRecoveryInterval -- never during a gesture or unfinished input. A session with
// nothing unsaved has no recovery file. A user close (Done with nothing unsaved, Discard, Save
// and close) deletes it; a crash or forced stop leaves it. The next session lists what it finds
// (newest first, at most kRecoveryOffers; files older than kRecoveryRetention are deleted) and
// offers each: Recover applies it as one undoable edit and leaves it unsaved (it is not a
// draft the user saved), Discard deletes it. Nothing is loaded without the user asking. The
// project the work belonged to is not known (no ACAPI read here); a recovered snapshot drawn
// on another terrain says so, as an opened draft does.
#include <chrono>
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
// #159 D2. Posts the session's next recovery action when one is due (the first call scans for
// earlier sessions' work); called from Follow on the message loop, `now` injectable for tests.
inline constexpr std::chrono::seconds kRecoveryQuiet { 2 }, kRecoveryInterval { 10 };
inline constexpr std::chrono::hours kRecoveryRetention { 24 * 14 };
inline constexpr size_t kRecoveryOffers = 5;
bool PostRecovery (Session& session, std::chrono::steady_clock::time_point now);
bool RequestRecover (Session& session, const std::string& file);
bool RequestDiscardRecovery (Session& session, const std::string& file);
// "Keep recovery snapshots": off deletes this session's recovery file.
void SetRecoveryEnabled (Session& session, bool enabled);
// The panel's draft row and the unsaved-work prompt (layout only; they request, never write).
void DraftControls (Session& session);
// #191 U2: the File menu's Save draft and Open draft... (inside an open menu or popup); the list
// Open asks for is drawn by DraftControls.
void DraftMenuItems (Session& session);
void ClosePrompt (Session& session);
} // namespace geomsrv::archviz::edithud
#endif
