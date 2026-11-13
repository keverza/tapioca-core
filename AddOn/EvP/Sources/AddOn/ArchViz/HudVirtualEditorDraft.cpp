#include "ArchViz/HudVirtualEditorDraft.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "Edit/EditDraft.hpp"
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

// Durable drafts (#159 D1) and recovery (D2). OVERLAY-INVARIANTS.md §8: everything here lives on
// the editor Session and ends with it -- the recovery FILE outlives it on purpose, the state about
// it does not; the file IO runs on the message loop, never in Present.
namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;
namespace fs = std::filesystem;

namespace {
std::string UtcNow ()
{
    const auto time = std::chrono::system_clock::to_time_t (std::chrono::system_clock::now ());
    std::tm utc {};
#ifdef _WIN32
    gmtime_s (&utc, &time);
#else
    gmtime_r (&time, &utc);
#endif
    std::ostringstream out;
    out << std::put_time (&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str ();
}

// Input that a save or open must not cut through: the document is mid-change.
bool Busy (const Session& s)
{
    return s.close || s.edits.InGesture () || !s.drawing.empty () || GizmoModeLocked (s) ||
           s.landscapeCommand.has_value () || s.terrainCapture != nullptr || s.draftingImport ||
           s.booleanDraft.has_value () || s.rectangularArray.has_value ();
}

std::string Utf8 (const fs::path& path)
{
    const auto text = path.u8string ();
    return std::string (text.begin (), text.end ());
}

bool Failed (Session& s, const std::string& error)
{
    s.message = error;
    ArchVizLog ("VIRTUAL EDITOR  draft: " + error);
    return false;
}

using Kind = Session::DraftRequest::Kind;

// Background recovery actions give way to anything the user asks for; they are posted again.
bool Background (const std::optional<Session::DraftRequest>& request)
{
    return request && (request->kind == Kind::Snapshot || request->kind == Kind::Scan);
}

bool Request (Session& s, Session::DraftRequest request)
{
    if (s.draftRequest && !Background (s.draftRequest))
        return Failed (s, "A draft file action is already running.");
    if (!s.edits.Doc ().landscape)
        return Failed (s, "Only a landscape session can be saved as a draft.");
    if (request.kind != Kind::List && request.kind != Kind::DiscardRecovery && Busy (s))
        return Failed (s, "Finish or Esc the current input first.");
    s.draftRequest = std::move (request);
    return true;
}

// The old file is replaced only by a complete new one: a temporary file, then a rename.
bool WriteAtomic (const fs::path& directory, const std::string& file, const std::string& text, std::string& error)
{
    const fs::path target = directory / fs::path (file);
    const fs::path temporary = directory / fs::path (file + ".partial");
    std::error_code code;
    fs::create_directories (directory, code);
    if (code) {
        error = "cannot create " + Utf8 (directory) + " (" + code.message () + ").";
        return false;
    }
    {
        std::ofstream out (temporary, std::ios::binary | std::ios::trunc);
        out.write (text.data (), std::streamsize (text.size ()));
        out.flush ();
        if (!out) {
            out.close ();
            fs::remove (temporary, code);
            error = "the file could not be written.";
            return false;
        }
    }
    fs::rename (temporary, target, code);
    if (code) {
        std::error_code ignored;
        fs::remove (temporary, ignored);
        error = code.message () + ".";
        return false;
    }
    return true;
}

fs::path RecoveryFolder (const fs::path& directory)
{
    return directory / "Recovery";
}

std::string RecoveryFile (const Session& s)
{
    return s.recoveryId + lg::kDraftExtension;
}

// Deletes this session's recovery file: nothing unsaved is left to recover.
void ForgetRecovery (Session& s, const fs::path& directory)
{
    if (!s.recoveryWritten || s.recoveryId.empty ())
        return;
    std::error_code code;
    fs::remove (RecoveryFolder (directory) / RecoveryFile (s), code);
    if (code)
        ArchVizLog ("VIRTUAL EDITOR  recovery: could not delete " + RecoveryFile (s) + " (" + code.message () + ")");
    s.recoveryWritten = false;
}

bool Save (Session& s, const fs::path& directory, const Session::DraftRequest& request)
{
    const edit::Document snapshot = s.edits.Doc ();
    const std::string savedAt = UtcNow ();
    std::string text, error;
    if (!lg::WriteDraft (snapshot, savedAt, text, error))
        return Failed (s, "Draft not saved: " + error);
    const fs::path target = directory / fs::path (request.file);
    if (!WriteAtomic (directory, request.file, text, error))
        return Failed (s, "Draft not saved: " + error);
    // What is on disk is the snapshot, whatever happened to the document since.
    s.draftClean = std::make_shared<const edit::Document> (snapshot);
    s.draftDirtyRevision = UINT64_MAX;
    s.draftFile = request.file;
    s.draftSavedAt = savedAt;
    if (std::find (s.draftFiles.begin (), s.draftFiles.end (), request.file) == s.draftFiles.end () &&
        s.draftFilesListed) {
        s.draftFiles.push_back (request.file);
        std::sort (s.draftFiles.begin (), s.draftFiles.end ());
    }
    s.message = "Draft saved: " + Utf8 (target) + ". No model writes.";
    ArchVizLog ("VIRTUAL EDITOR  draft saved: " + Utf8 (target));
    // Saved: nothing unsaved is left to recover (until the next change).
    ForgetRecovery (s, directory);
    s.recoveryRevision = snapshot.revision;
    if (request.closeAfter) {
        s.closePrompt = false;
        s.close = true;
    }
    return true;
}

bool ReadFile (const fs::path& path, std::string& text)
{
    std::ifstream in (path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream content;
    content << in.rdbuf ();
    text = content.str ();
    return true;
}

bool Open (Session& s, const fs::path& directory, const Session::DraftRequest& request)
{
    if (Busy (s))
        return Failed (s, "The editor was busy; open the draft again.");
    const fs::path path = directory / fs::path (request.file);
    std::ifstream in (path, std::ios::binary);
    if (!in)
        return Failed (s, "Draft not opened: " + Utf8 (path) + " cannot be read.");
    std::ostringstream content;
    content << in.rdbuf ();
    edit::Document loaded;
    lg::DraftInfo info;
    std::string error;
    if (!lg::ReadDraft (content.str (), loaded, info, error))
        return Failed (s, "Draft not opened: " + error);
    const std::string terrainBefore = s.edits.Doc ().landscape ? s.edits.Doc ().landscape->sourceTerrainGuid : "";
    // One undoable edit, as a session snapshot's restore is: never a second history, never an
    // allocator reset, so ids handed out in this session are not reused.
    if (!s.edits.Apply ([loaded] (edit::Document& doc, std::string&) {
            const auto highWater = doc.nextId;
            doc = loaded;
            doc.nextId = std::max (doc.nextId, highWater);
            return true;
        }))
        return Failed (s, "Draft not opened: " + s.edits.Message ());
    s.selection.Clear ();
    s.landscapeSet = s.landscapeOperandSet = edit::kNoId;
    Landscape (s);
    s.draftClean = std::make_shared<const edit::Document> (s.edits.Doc ());
    s.draftDirtyRevision = UINT64_MAX;
    s.draftFile = request.file;
    s.draftSavedAt = info.savedUtc;
    s.draftName = request.file.substr (0, request.file.size () - std::strlen (lg::kDraftExtension));
    ForgetRecovery (s, directory); // the opened draft is the clean state now
    s.recoveryRevision = s.edits.Doc ().revision;
    s.message = "Draft opened (saved " + info.savedUtc + "). Undo returns to the work it replaced.";
    if (s.edits.Doc ().landscape && s.edits.Doc ().landscape->sourceTerrainGuid != terrainBefore)
        s.message += " It was drawn on another terrain: capture the terrain again before grading.";
    ArchVizLog ("VIRTUAL EDITOR  draft opened: " + Utf8 (path));
    return true;
}

bool List (Session& s, const fs::path& directory)
{
    s.draftFiles.clear ();
    s.draftFilesListed = true;
    std::error_code code;
    if (!fs::exists (directory, code))
        return true;
    const std::string extension = lg::kDraftExtension;
    for (fs::directory_iterator it (directory, code), end; !code && it != end; it.increment (code)) {
        if (!it->is_regular_file (code))
            continue;
        const std::string name = Utf8 (it->path ().filename ());
        if (name.size () > extension.size () &&
            name.compare (name.size () - extension.size (), extension.size (), extension) == 0)
            s.draftFiles.push_back (name);
    }
    std::sort (s.draftFiles.begin (), s.draftFiles.end ());
    if (code)
        return Failed (s, "The drafts folder could not be listed: " + code.message () + ".");
    return true;
}

// #159 D2 -----------------------------------------------------------------------------------

// A snapshot of the last committed revision while there is unsaved work; none without.
bool Snapshot (Session& s, const fs::path& directory)
{
    const edit::Document& doc = s.edits.Doc ();
    if (!s.recoveryEnabled || !doc.landscape || Busy (s))
        return false;
    if (!DraftDirty (s)) {
        ForgetRecovery (s, directory);
        s.recoveryRevision = doc.revision;
        s.recoveryStatus = "Nothing unsaved to recover.";
        return true;
    }
    const std::string savedAt = UtcNow ();
    std::string text, error;
    if (!lg::WriteDraft (doc, savedAt, text, error) ||
        !WriteAtomic (RecoveryFolder (directory), RecoveryFile (s), text, error)) {
        s.recoveryStatus = "Recovery snapshot failed: " + error;
        ArchVizLog ("VIRTUAL EDITOR  recovery: " + s.recoveryStatus);
        return false;
    }
    s.recoveryWritten = true;
    s.recoveryRevision = doc.revision;
    s.recoveryStatus = "Recovery snapshot " + savedAt + " (revision " + std::to_string (doc.revision) + ").";
    return true;
}

// Earlier sessions' recovery files, newest first. Expired ones are deleted; unreadable ones are
// logged and left for a later version to read.
bool Scan (Session& s, const fs::path& directory)
{
    s.recoveryScanned = true;
    s.recoveries.clear ();
    const fs::path folder = RecoveryFolder (directory);
    std::error_code code;
    if (!fs::exists (folder, code))
        return true;
    const std::string extension = lg::kDraftExtension;
    const auto now = fs::file_time_type::clock::now ();
    std::vector<std::pair<fs::file_time_type, Session::RecoveryOffer>> found;
    for (fs::directory_iterator it (folder, code), end; !code && it != end; it.increment (code)) {
        std::error_code entry;
        if (!it->is_regular_file (entry))
            continue;
        const std::string name = Utf8 (it->path ().filename ());
        if (name.size () <= extension.size () ||
            name.compare (name.size () - extension.size (), extension.size (), extension) != 0 ||
            name == RecoveryFile (s))
            continue;
        const auto written = it->last_write_time (entry);
        if (entry)
            continue;
        if (now - written > kRecoveryRetention) {
            fs::remove (it->path (), entry);
            ArchVizLog ("VIRTUAL EDITOR  recovery: expired " + name);
            continue;
        }
        std::string text, error;
        edit::Document doc;
        lg::DraftInfo info;
        if (!ReadFile (it->path (), text) || !lg::ReadDraft (text, doc, info, error)) {
            ArchVizLog ("VIRTUAL EDITOR  recovery: " + name + " not offered: " + error);
            continue;
        }
        Session::RecoveryOffer offer;
        offer.file = name;
        offer.savedUtc = info.savedUtc;
        offer.revision = info.revision;
        offer.objects = doc.entities.size () + doc.paths.size ();
        offer.terrain = doc.landscape ? doc.landscape->sourceTerrainGuid : "";
        found.emplace_back (written, std::move (offer));
    }
    std::sort (found.begin (), found.end (), [] (const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second.file < b.second.file;
    });
    for (auto& entry : found)
        if (s.recoveries.size () < kRecoveryOffers)
            s.recoveries.push_back (std::move (entry.second));
    if (code)
        ArchVizLog ("VIRTUAL EDITOR  recovery: the folder could not be listed: " + code.message ());
    return true;
}

void DropOffer (Session& s, const std::string& file)
{
    s.recoveries.erase (std::remove_if (s.recoveries.begin (), s.recoveries.end (),
                                        [&] (const Session::RecoveryOffer& o) { return o.file == file; }),
                        s.recoveries.end ());
}

// The recovered work becomes this session's unsaved work: one undoable edit, not a save.
bool Recover (Session& s, const fs::path& directory, const Session::DraftRequest& request)
{
    if (Busy (s))
        return Failed (s, "The editor was busy; recover the work again.");
    const fs::path path = RecoveryFolder (directory) / fs::path (request.file);
    std::string text, error;
    edit::Document loaded;
    lg::DraftInfo info;
    if (!ReadFile (path, text))
        return Failed (s, "Not recovered: " + Utf8 (path) + " cannot be read.");
    if (!lg::ReadDraft (text, loaded, info, error))
        return Failed (s, "Not recovered: " + error);
    const std::string terrainBefore = s.edits.Doc ().landscape ? s.edits.Doc ().landscape->sourceTerrainGuid : "";
    if (!s.edits.Apply ([loaded] (edit::Document& doc, std::string&) {
            const auto highWater = doc.nextId;
            doc = loaded;
            doc.nextId = std::max (doc.nextId, highWater);
            return true;
        }))
        return Failed (s, "Not recovered: " + s.edits.Message ());
    s.selection.Clear ();
    s.landscapeSet = s.landscapeOperandSet = edit::kNoId;
    Landscape (s);
    // This session's own snapshots take over from here.
    std::error_code code;
    fs::remove (path, code);
    DropOffer (s, request.file);
    s.message = "Recovered the work from " + info.savedUtc + " (revision " + std::to_string (info.revision) +
                "). It is not saved yet: save it as a draft. Undo returns to the work it replaced.";
    if (s.edits.Doc ().landscape && s.edits.Doc ().landscape->sourceTerrainGuid != terrainBefore)
        s.message += " It was drawn on another terrain: capture the terrain again before grading.";
    ArchVizLog ("VIRTUAL EDITOR  recovered: " + Utf8 (path));
    return true;
}

bool DiscardRecovery (Session& s, const fs::path& directory, const Session::DraftRequest& request)
{
    std::error_code code;
    fs::remove (RecoveryFolder (directory) / fs::path (request.file), code);
    if (code)
        return Failed (s, "The recovered work could not be deleted: " + code.message () + ".");
    DropOffer (s, request.file);
    s.message = "Recovered work discarded.";
    return true;
}

// A user close leaves nothing to recover. Posted directly (Busy refuses while s.close is set) and
// in place of a pending background action; the hosts drain it before they stop the editor.
void PostForget (Session& s)
{
    if (s.recoveryWritten && (!s.draftRequest || Background (s.draftRequest)))
        s.draftRequest = Session::DraftRequest { Kind::Forget, {}, false };
}
} // namespace

bool DraftDirty (Session& s)
{
    const auto& doc = s.edits.Doc ();
    if (s.draftDirtyRevision != doc.revision) {
        s.draftDirty = !s.draftClean || !edit::SameAuthoredContent (doc, *s.draftClean);
        s.draftDirtyRevision = doc.revision;
    }
    return s.draftDirty;
}

void RequestClose (Session& s)
{
    if (s.edits.Doc ().landscape && DraftDirty (s)) {
        s.closePrompt = true;
        s.message = "Unsaved changes: save the draft, discard them, or cancel.";
        return;
    }
    s.close = true;
    PostForget (s);
}

bool RequestDraftSave (Session& s, bool closeAfter)
{
    const std::string file = lg::DraftFileName (s.draftName);
    if (file.empty ())
        return Failed (s, "Name the draft first (letters, digits, spaces, - or _).");
    return Request (s, { Session::DraftRequest::Kind::Save, file, closeAfter });
}

bool RequestDraftOpen (Session& s, const std::string& file)
{
    if (lg::DraftFileName (file) != file)
        return Failed (s, "That is not a draft file name.");
    return Request (s, { Session::DraftRequest::Kind::Open, file, false });
}

bool RequestDraftList (Session& s)
{
    return Request (s, { Session::DraftRequest::Kind::List, {}, false });
}

bool RequestRecover (Session& s, const std::string& file)
{
    if (lg::DraftFileName (file) != file)
        return Failed (s, "That is not a recovery file name.");
    return Request (s, { Kind::Recover, file, false });
}

bool RequestDiscardRecovery (Session& s, const std::string& file)
{
    if (lg::DraftFileName (file) != file)
        return Failed (s, "That is not a recovery file name.");
    return Request (s, { Kind::DiscardRecovery, file, false });
}

void SetRecoveryEnabled (Session& s, bool enabled)
{
    s.recoveryEnabled = enabled;
    if (enabled) {
        s.recoveryRevision = UINT64_MAX; // write the current work at the next quiet moment
        s.recoveryAttempt = {};
        s.recoveryStatus.clear ();
        return;
    }
    s.recoveryStatus = "Recovery snapshots are off.";
    PostForget (s);
}

bool PostRecovery (Session& s, std::chrono::steady_clock::time_point now)
{
    if (s.draftRequest || s.close || !s.edits.Doc ().landscape)
        return false;
    if (s.recoveryId.empty ()) {
        std::random_device device;
        std::mt19937_64 random ((uint64_t (device ()) << 32) ^ device () ^
                                uint64_t (std::chrono::system_clock::now ().time_since_epoch ().count ()));
        char id[24];
        std::snprintf (id, sizeof id, "%016llx", static_cast<unsigned long long> (random ()));
        s.recoveryId = id;
    }
    if (!s.recoveryScanned) {
        s.draftRequest = Session::DraftRequest { Kind::Scan, {}, false };
        return true;
    }
    const uint64_t revision = s.edits.Doc ().revision;
    if (revision != s.recoverySeenRevision) {
        s.recoverySeenRevision = revision;
        s.recoveryChanged = now;
    }
    // Committed and quiet: never half a gesture, never while a change is still arriving.
    const bool attempted = s.recoveryAttempt != std::chrono::steady_clock::time_point {};
    if (!s.recoveryEnabled || revision == s.recoveryRevision || Busy (s) || now - s.recoveryChanged < kRecoveryQuiet ||
        (attempted && now - s.recoveryAttempt < kRecoveryInterval))
        return false;
    s.recoveryAttempt = now;
    s.draftRequest = Session::DraftRequest { Kind::Snapshot, {}, false };
    return true;
}

bool FollowDraftFiles (Session& s, const fs::path& directory)
{
    if (!s.draftRequest)
        return false;
    const Session::DraftRequest request = *s.draftRequest;
    s.draftRequest.reset ();
    try {
        switch (request.kind) {
            case Session::DraftRequest::Kind::Save:
                return Save (s, directory, request);
            case Session::DraftRequest::Kind::Open:
                return Open (s, directory, request);
            case Session::DraftRequest::Kind::List:
                return List (s, directory);
            case Kind::Snapshot:
                return Snapshot (s, directory);
            case Kind::Scan:
                return Scan (s, directory);
            case Kind::Recover:
                return Recover (s, directory, request);
            case Kind::DiscardRecovery:
                return DiscardRecovery (s, directory, request);
            case Kind::Forget:
                ForgetRecovery (s, directory);
                return true;
        }
    }
    catch (const std::exception& exception) {
        return Failed (s, std::string ("Draft file action failed: ") + exception.what ());
    }
    return false;
}

bool FollowDraftFiles (Session& s)
{
    if (!s.draftRequest)
        return false;
#ifdef _WIN32
    const auto* root = _wgetenv (L"LOCALAPPDATA");
#else
    const auto* root = std::getenv ("LOCALAPPDATA");
#endif
    if (!root || !*root) {
        s.draftRequest.reset ();
        return Failed (s, "LOCALAPPDATA is unavailable; drafts cannot be stored.");
    }
    return FollowDraftFiles (s, fs::path (root) / "Tapioca" / "Drafts");
}

void DraftControls (Session& s)
{
    if (!s.edits.Doc ().landscape)
        return;
    const bool dirty = DraftDirty (s);
    const bool pending = s.draftRequest.has_value () && !Background (s.draftRequest);
    ImGui::Separator ();
    // #159 D2: earlier sessions' unsaved work, offered, never loaded unasked.
    if (!s.recoveries.empty ()) {
        ImGui::TextWrapped ("Unsaved work from an earlier session was found:");
        const std::string terrain = s.edits.Doc ().landscape->sourceTerrainGuid;
        ImGui::BeginDisabled (pending || Busy (s));
        for (const auto offer : s.recoveries) { // a copy: a click may drop it from the list
            ImGui::PushID (offer.file.c_str ());
            ImGui::BulletText ("%s, revision %llu, %zu objects%s", offer.savedUtc.c_str (),
                               static_cast<unsigned long long> (offer.revision), offer.objects,
                               offer.terrain != terrain ? ", another terrain" : "");
            ImGui::SameLine ();
            if (ImGui::SmallButton ("Recover"))
                RequestRecover (s, offer.file);
            if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
                geomsrv::archviz::hudshell::HoverTip (
                    "Replace the current work with it as one undoable edit. It stays unsaved until you save "
                    "a draft.");
            ImGui::SameLine ();
            if (ImGui::SmallButton ("Discard"))
                RequestDiscardRecovery (s, offer.file);
            if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
                geomsrv::archviz::hudshell::HoverTip ("Delete this recovered work.");
            ImGui::PopID ();
        }
        ImGui::EndDisabled ();
        ImGui::Separator ();
    }
    ImGui::TextUnformatted ("Draft");
    ImGui::SameLine ();
    if (dirty)
        ImGui::TextDisabled ("unsaved changes");
    else if (!s.draftFile.empty ())
        ImGui::TextDisabled ("saved %s", s.draftSavedAt.c_str ());
    else
        ImGui::TextDisabled ("nothing to save yet");
    // #191 U2: Save draft and Open draft... are in the command bar's File menu (DraftMenuItems);
    // the name they save under, and the list Open shows, stay here.
    char name[96] = {};
    std::strncpy (name, s.draftName.c_str (), sizeof name - 1);
    ImGui::SetNextItemWidth (180.0f);
    if (ImGui::InputText ("##draft-name", name, sizeof name))
        s.draftName = name;
    bool keep = s.recoveryEnabled;
    if (ImGui::Checkbox ("Keep recovery snapshots", &keep))
        SetRecoveryEnabled (s, keep);
    if (ImGui::IsItemHovered ())
        geomsrv::archviz::hudshell::HoverTip (
            "While there is unsaved work, a copy of the last finished change is kept in "
            "%%LOCALAPPDATA%%\\Tapioca\\Drafts\\Recovery and offered if the editor ends without closing. "
            "Closing normally deletes it.");
    if (!s.recoveryStatus.empty ()) {
        ImGui::SameLine ();
        ImGui::TextDisabled ("%s", s.recoveryStatus.c_str ());
    }
    if (!s.draftFilesListed)
        return;
    if (s.draftFiles.empty ())
        ImGui::TextDisabled ("No drafts saved yet.");
    ImGui::BeginDisabled (pending || Busy (s));
    for (const auto& file : s.draftFiles) {
        ImGui::PushID (file.c_str ());
        if (ImGui::Selectable (file.c_str (), file == s.draftFile))
            RequestDraftOpen (s, file);
        ImGui::PopID ();
    }
    ImGui::EndDisabled ();
    ImGui::BeginDisabled (pending || Busy (s));
    if (ImGui::SmallButton ("Refresh drafts"))
        RequestDraftList (s);
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Hide drafts"))
        s.draftFilesListed = false, s.draftFiles.clear ();
}

void DraftMenuItems (Session& s)
{
    const bool idle = s.edits.Doc ().landscape && !(s.draftRequest && !Background (s.draftRequest)) && !Busy (s);
    if (ImGui::MenuItem ("Save draft", nullptr, false, idle))
        RequestDraftSave (s);
    if (ImGui::MenuItem ("Open draft...", nullptr, false, idle))
        RequestDraftList (s);
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        geomsrv::archviz::hudshell::HoverTip (
            "Drafts are files in %%LOCALAPPDATA%%\\Tapioca\\Drafts. Opening one is a single "
            "undoable edit; undo history itself is not saved. Nothing is written to the model.");
}

void ClosePrompt (Session& s)
{
    if (!s.closePrompt)
        return;
    ImGui::TextWrapped ("This draft has unsaved changes.");
    ImGui::BeginDisabled (s.draftRequest.has_value ());
    if (ImGui::SmallButton ("Save and close"))
        RequestDraftSave (s, true);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Discard and close")) {
        s.closePrompt = false, s.close = true;
        PostForget (s); // discarded on purpose: nothing to recover
    }
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Cancel"))
        s.closePrompt = false, s.message.clear ();
    ImGui::EndDisabled ();
    ImGui::Separator ();
}
} // namespace geomsrv::archviz::edithud
