// Durable drafts in the editor session (HudVirtualEditorDraft.hpp, #159 D1): unsaved-work
// tracking, Save / Open through the message-loop drain into a temporary folder, the close
// prompt, and every failure leaving the session as it was.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDraft.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "Edit/EditDraft.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditSession.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <fstream>

namespace ed = geomsrv::edit;
namespace eh = geomsrv::archviz::edithud;
namespace geo = tapioca::geometry;
namespace hud = geomsrv::archviz::overlayhud;
namespace fs = std::filesystem;

namespace {
struct Folder {
    fs::path path;
    Folder ()
    {
        static std::atomic<int> counter { 0 };
        path = fs::temp_directory_path () /
               ("tapioca-drafts-" + std::to_string (::testing::UnitTest::GetInstance ()->random_seed ()) + "-" +
                std::to_string (counter++) + "-" + ::testing::UnitTest::GetInstance ()->current_test_info ()->name ());
        std::error_code ignored;
        fs::remove_all (path, ignored);
    }
    ~Folder ()
    {
        std::error_code ignored;
        fs::remove_all (path, ignored);
    }
};

ed::Document Started ()
{
    ed::Document doc;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, error));
    return doc;
}

// A drawn reference rectangle, as the editor's default drawing registers it: one undo step.
ed::Id Draw (eh::Session& s, double x)
{
    ed::Id made = ed::kNoId;
    EXPECT_TRUE (s.edits.Apply ([&] (ed::Document& d, std::string& error) {
        made = ed::AddPolyline (d, { { x, 0, 0 }, { x + 4, 0, 0 }, { x + 4, 4, 0 }, { x, 4, 0 } }, true);
        return geo::SetLandscapeSourceRole (d, made, geo::SourceRole::ReferenceLine, error);
    })) << s.edits.Message ();
    return made;
}

void Escape (eh::Session& s)
{
    hud::Input input = hudtest::At (0, 0);
    input.viewActive = true;
    input.keys = { { 0x1B, true } };
    eh::Keys (s, input, false);
}
} // namespace

TEST (HudVirtualEditorDraft, SaveStopAndReopenRestoresTheAuthoredWorkAndItsIds)
{
    Folder folder;
    ed::Document saved;
    {
        eh::Session s (Started ());
        EXPECT_FALSE (eh::DraftDirty (s)) << "a fresh session has nothing to save";
        Draw (s, 0);
        Draw (s, 10);
        EXPECT_TRUE (eh::DraftDirty (s));
        s.draftName = "Site A";
        ASSERT_TRUE (eh::RequestDraftSave (s)) << s.message;
        EXPECT_TRUE (!fs::exists (folder.path) || fs::is_empty (folder.path)) << "layout must not write";
        ASSERT_TRUE (eh::FollowDraftFiles (s, folder.path)) << s.message;
        EXPECT_TRUE (fs::exists (folder.path / "Site A.tapdraft"));
        EXPECT_FALSE (fs::exists (folder.path / "Site A.tapdraft.partial"));
        EXPECT_FALSE (eh::DraftDirty (s));
        EXPECT_EQ (s.draftFile, "Site A.tapdraft");
        EXPECT_EQ (s.edits.UndoDepth (), 2u) << "saving writes no history";
        saved = s.edits.Doc ();
    } // the editor stops: its session is gone
    eh::Session s (Started ());
    ASSERT_TRUE (eh::RequestDraftList (s));
    ASSERT_TRUE (eh::FollowDraftFiles (s, folder.path)) << s.message;
    ASSERT_EQ (s.draftFiles, std::vector<std::string> { "Site A.tapdraft" });
    ASSERT_TRUE (eh::RequestDraftOpen (s, "Site A.tapdraft")) << s.message;
    ASSERT_TRUE (eh::FollowDraftFiles (s, folder.path)) << s.message;
    EXPECT_TRUE (ed::SameAuthoredContent (s.edits.Doc (), saved));
    EXPECT_EQ (s.edits.Doc ().entities, saved.entities);
    EXPECT_EQ (s.edits.Doc ().landscape, saved.landscape);
    EXPECT_GE (s.edits.Doc ().nextId, saved.nextId);
    EXPECT_FALSE (eh::DraftDirty (s));
    EXPECT_EQ (s.draftName, "Site A");
    // Opening is one undoable edit: the work it replaced is one Undo away.
    EXPECT_EQ (s.edits.UndoDepth (), 1u);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_TRUE (s.edits.Doc ().entities.empty ());
    EXPECT_TRUE (eh::DraftDirty (s)) << "undoing the open differs from the opened draft";
}

TEST (HudVirtualEditorDraft, EditingAfterASaveIsUnsavedAgainAndUndoingBackIsNot)
{
    Folder folder;
    eh::Session s (Started ());
    Draw (s, 0);
    ASSERT_TRUE (eh::RequestDraftSave (s));
    ASSERT_TRUE (eh::FollowDraftFiles (s, folder.path)) << s.message;
    Draw (s, 10);
    EXPECT_TRUE (eh::DraftDirty (s));
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_FALSE (eh::DraftDirty (s)) << "content, not the revision counter, decides";
}

TEST (HudVirtualEditorDraft, AFailedWriteKeepsTheWorkUnsavedAndTheOldFile)
{
    Folder folder;
    eh::Session s (Started ());
    Draw (s, 0);
    ASSERT_TRUE (eh::RequestDraftSave (s));
    ASSERT_TRUE (eh::FollowDraftFiles (s, folder.path));
    const auto before = fs::file_size (folder.path / "Untitled.tapdraft");
    Draw (s, 10);
    // The drafts folder's place is taken by a file: nothing can be written.
    const fs::path blocked = folder.path / "blocked";
    std::ofstream (blocked) << "x";
    ASSERT_TRUE (eh::RequestDraftSave (s));
    EXPECT_FALSE (eh::FollowDraftFiles (s, blocked / "drafts"));
    EXPECT_NE (s.message.find ("not saved"), std::string::npos) << s.message;
    EXPECT_TRUE (eh::DraftDirty (s));
    EXPECT_EQ (fs::file_size (folder.path / "Untitled.tapdraft"), before);
    EXPECT_FALSE (s.draftRequest.has_value ());
}

TEST (HudVirtualEditorDraft, AMalformedOrMissingDraftChangesNothing)
{
    Folder folder;
    fs::create_directories (folder.path);
    std::ofstream (folder.path / "Broken.tapdraft") << "{\"format\":\"tapioca.virtual-editor.draft\"";
    eh::Session s (Started ());
    Draw (s, 0);
    const ed::Document before = s.edits.Doc ();
    for (const char* file : { "Broken.tapdraft", "Missing.tapdraft" }) {
        ASSERT_TRUE (eh::RequestDraftOpen (s, file)) << s.message;
        EXPECT_FALSE (eh::FollowDraftFiles (s, folder.path));
        EXPECT_NE (s.message.find ("not opened"), std::string::npos) << s.message;
        EXPECT_EQ (s.edits.Doc ().entities, before.entities);
        EXPECT_EQ (s.edits.Doc ().revision, before.revision);
        EXPECT_EQ (s.edits.UndoDepth (), 1u);
        EXPECT_TRUE (eh::DraftDirty (s));
    }
    EXPECT_FALSE (eh::RequestDraftOpen (s, "..\\elsewhere.tapdraft")) << "only names inside the drafts folder";
}

TEST (HudVirtualEditorDraft, UnfinishedInputOrAPendingActionRefusesANewRequest)
{
    eh::Session s (Started ());
    Draw (s, 0);
    ASSERT_TRUE (s.edits.Begin ());
    EXPECT_FALSE (eh::RequestDraftSave (s));
    EXPECT_FALSE (s.draftRequest.has_value ());
    s.edits.Cancel ();
    ASSERT_TRUE (eh::RequestDraftSave (s));
    EXPECT_FALSE (eh::RequestDraftList (s)) << "one file action at a time";
    s.draftName = "///";
    s.draftRequest.reset ();
    EXPECT_FALSE (eh::RequestDraftSave (s));
    eh::Session plain; // no landscape: nothing a draft can hold
    EXPECT_FALSE (eh::RequestDraftSave (plain));
}

TEST (HudVirtualEditorDraft, ClosingWithUnsavedWorkAsksAndEachAnswerDoesWhatItSays)
{
    Folder folder;
    {
        eh::Session s (Started ());
        eh::RequestClose (s);
        EXPECT_TRUE (s.close) << "nothing unsaved: Done closes at once";
    }
    {
        eh::Session s (Started ());
        Draw (s, 0);
        eh::RequestClose (s);
        EXPECT_FALSE (s.close);
        EXPECT_TRUE (s.closePrompt);
        Escape (s); // Esc is the prompt's Cancel, and nothing more
        EXPECT_FALSE (s.closePrompt);
        EXPECT_FALSE (s.close);
        EXPECT_EQ (s.edits.Doc ().entities.size (), 1u);
    }
    {
        eh::Session s (Started ());
        Draw (s, 0);
        eh::RequestClose (s);
        ASSERT_TRUE (eh::RequestDraftSave (s, true));
        EXPECT_FALSE (s.close) << "closes only once the file is written";
        ASSERT_TRUE (eh::FollowDraftFiles (s, folder.path)) << s.message;
        EXPECT_TRUE (s.close);
        EXPECT_FALSE (s.closePrompt);
        EXPECT_TRUE (fs::exists (folder.path / "Untitled.tapdraft"));
    }
    {
        // Save-and-close that fails stays open, still asking.
        eh::Session s (Started ());
        Draw (s, 0);
        eh::RequestClose (s);
        const fs::path blocked = folder.path / "blocked";
        std::ofstream (blocked) << "x";
        ASSERT_TRUE (eh::RequestDraftSave (s, true));
        EXPECT_FALSE (eh::FollowDraftFiles (s, blocked / "drafts"));
        EXPECT_FALSE (s.close);
        EXPECT_TRUE (s.closePrompt);
    }
}

TEST (HudVirtualEditorDraft, TheSessionSnapshotIsNotADurableSave)
{
    eh::Session s (Started ());
    Draw (s, 0);
    std::string error;
    ASSERT_TRUE (geo::WriteLandscape (s.edits.Doc (), geo::LandscapeStorage::Virtual, s.landscapeSaved, error));
    EXPECT_TRUE (eh::DraftDirty (s)) << "an in-memory snapshot leaves the work unsaved";
}
