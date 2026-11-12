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
#include <sstream>
#include <system_error>

// Durable drafts (#159 D1). OVERLAY-INVARIANTS.md §8: everything here lives on the editor
// Session and ends with it; the file IO runs on the message loop, never in Present.
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

bool Request (Session& s, Session::DraftRequest request)
{
    if (s.draftRequest)
        return Failed (s, "A draft file action is already running.");
    if (!s.edits.Doc ().landscape)
        return Failed (s, "Only a landscape session can be saved as a draft.");
    if (request.kind != Session::DraftRequest::Kind::List && Busy (s))
        return Failed (s, "Finish or Esc the current input first.");
    s.draftRequest = std::move (request);
    return true;
}

bool Save (Session& s, const fs::path& directory, const Session::DraftRequest& request)
{
    const edit::Document snapshot = s.edits.Doc ();
    const std::string savedAt = UtcNow ();
    std::string text, error;
    if (!lg::WriteDraft (snapshot, savedAt, text, error))
        return Failed (s, "Draft not saved: " + error);
    const fs::path target = directory / fs::path (request.file);
    const fs::path temporary = directory / fs::path (request.file + ".partial");
    std::error_code code;
    fs::create_directories (directory, code);
    if (code)
        return Failed (s, "Draft not saved: cannot create " + Utf8 (directory) + " (" + code.message () + ").");
    {
        std::ofstream out (temporary, std::ios::binary | std::ios::trunc);
        out.write (text.data (), std::streamsize (text.size ()));
        out.flush ();
        if (!out) {
            out.close ();
            fs::remove (temporary, code);
            return Failed (s, "Draft not saved: the file could not be written.");
        }
    }
    // The old draft is replaced only by a complete new one.
    fs::rename (temporary, target, code);
    if (code) {
        std::error_code ignored;
        fs::remove (temporary, ignored);
        return Failed (s, "Draft not saved: " + code.message () + ".");
    }
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
    if (request.closeAfter) {
        s.closePrompt = false;
        s.close = true;
    }
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
    const bool pending = s.draftRequest.has_value ();
    ImGui::Separator ();
    ImGui::TextUnformatted ("Draft");
    ImGui::SameLine ();
    if (dirty)
        ImGui::TextDisabled ("unsaved changes");
    else if (!s.draftFile.empty ())
        ImGui::TextDisabled ("saved %s", s.draftSavedAt.c_str ());
    else
        ImGui::TextDisabled ("nothing to save yet");
    char name[96] = {};
    std::strncpy (name, s.draftName.c_str (), sizeof name - 1);
    ImGui::SetNextItemWidth (180.0f);
    if (ImGui::InputText ("##draft-name", name, sizeof name))
        s.draftName = name;
    ImGui::SameLine ();
    ImGui::BeginDisabled (pending || Busy (s));
    if (ImGui::SmallButton ("Save draft"))
        RequestDraftSave (s);
    ImGui::SameLine ();
    if (ImGui::SmallButton (s.draftFilesListed ? "Refresh drafts" : "Open draft..."))
        RequestDraftList (s);
    ImGui::EndDisabled ();
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        geomsrv::archviz::hudshell::HoverTip (
            "Drafts are files in %%LOCALAPPDATA%%\\Tapioca\\Drafts. Opening one is a single "
            "undoable edit; undo history itself is not saved. Nothing is written to the model.");
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
    if (ImGui::SmallButton ("Hide drafts"))
        s.draftFilesListed = false, s.draftFiles.clear ();
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
    if (ImGui::SmallButton ("Discard and close"))
        s.closePrompt = false, s.close = true;
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Cancel"))
        s.closePrompt = false, s.message.clear ();
    ImGui::EndDisabled ();
    ImGui::Separator ();
}
} // namespace geomsrv::archviz::edithud
