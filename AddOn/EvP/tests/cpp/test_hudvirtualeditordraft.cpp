// Durable drafts in the editor session (HudVirtualEditorDraft.hpp, #159 D1): unsaved-work
// tracking, Save / Open through the message-loop drain into a temporary folder, the close
// prompt, and every failure leaving the session as it was. D2: recovery snapshots of unsaved work,
// offered to the next session.
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
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

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

// ---- #159 D2 recovery -------------------------------------------------------------------------
namespace {
using Clock = std::chrono::steady_clock;
const Clock::time_point kT0 = Clock::time_point {} + std::chrono::hours (1);

// Posts what is due at `now` and drains it into `folder`, as a host tick does.
bool Tick (eh::Session& s, const Folder& folder, Clock::time_point now)
{
    if (!eh::PostRecovery (s, now))
        return false;
    eh::FollowDraftFiles (s, folder.path);
    return true;
}

fs::path OwnRecovery (const eh::Session& s, const Folder& folder)
{
    return folder.path / "Recovery" / (s.recoveryId + geo::kDraftExtension);
}

ed::Document ReadBack (const fs::path& path)
{
    std::ifstream in (path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf ();
    ed::Document doc;
    geo::DraftInfo info;
    std::string error;
    EXPECT_TRUE (geo::ReadDraft (text.str (), doc, info, error)) << error;
    return doc;
}
} // namespace

// A snapshot is written only for committed, unsaved work, after input has been quiet, at most
// every interval, never during a gesture; a save removes it; a clean close deletes it.
TEST (HudVirtualEditorDraft, RecoverySnapshotsCommittedUnsavedWorkWhenQuietAndAUserCloseDeletesIt159)
{
    Folder folder;
    eh::Session s (Started ());
    ASSERT_TRUE (Tick (s, folder, kT0)) << "the first tick looks for earlier sessions' work";
    EXPECT_TRUE (s.recoveryScanned);
    EXPECT_TRUE (s.recoveries.empty ());
    EXPECT_FALSE (s.recoveryId.empty ());
    EXPECT_FALSE (Tick (s, folder, kT0 + std::chrono::seconds (30))) << "nothing unsaved, nothing written";
    EXPECT_FALSE (fs::exists (OwnRecovery (s, folder)));

    const auto t1 = kT0 + std::chrono::seconds (60);
    Draw (s, 0);
    EXPECT_FALSE (Tick (s, folder, t1)) << "the change has just arrived";
    EXPECT_FALSE (Tick (s, folder, t1 + std::chrono::seconds (1)));
    ASSERT_TRUE (s.edits.Begin ());
    EXPECT_FALSE (Tick (s, folder, t1 + std::chrono::seconds (5))) << "never half a gesture";
    s.edits.Cancel ();
    ASSERT_TRUE (Tick (s, folder, t1 + std::chrono::seconds (8))) << s.recoveryStatus;
    ASSERT_TRUE (fs::exists (OwnRecovery (s, folder)));
    EXPECT_FALSE (fs::exists (OwnRecovery (s, folder).string () + ".partial"));
    EXPECT_TRUE (ed::SameAuthoredContent (ReadBack (OwnRecovery (s, folder)), s.edits.Doc ()));
    EXPECT_EQ (s.edits.UndoDepth (), 1u) << "a snapshot records no history";
    EXPECT_TRUE (eh::DraftDirty (s)) << "a recovery snapshot is not a save";
    EXPECT_FALSE (Tick (s, folder, t1 + std::chrono::seconds (60))) << "the same revision is not written again";

    const auto t2 = t1 + std::chrono::seconds (100);
    Draw (s, 10);
    EXPECT_FALSE (Tick (s, folder, t2));
    ASSERT_TRUE (Tick (s, folder, t2 + std::chrono::seconds (3)));
    EXPECT_EQ (ReadBack (OwnRecovery (s, folder)).entities.size (), 2u);
    Draw (s, 20);
    EXPECT_FALSE (Tick (s, folder, t2 + std::chrono::seconds (4)));
    EXPECT_FALSE (Tick (s, folder, t2 + std::chrono::seconds (8))) << "at most one snapshot per interval";
    ASSERT_TRUE (Tick (s, folder, t2 + std::chrono::seconds (14)));
    EXPECT_EQ (ReadBack (OwnRecovery (s, folder)).entities.size (), 3u);

    // A user request takes the slot from a pending background action.
    Draw (s, 30);
    ASSERT_FALSE (eh::PostRecovery (s, t2 + std::chrono::seconds (40)));
    ASSERT_TRUE (eh::PostRecovery (s, t2 + std::chrono::seconds (43)));
    ASSERT_TRUE (s.draftRequest.has_value ()) << "a snapshot is pending";
    s.draftName = "Kept";
    ASSERT_TRUE (eh::RequestDraftSave (s)) << s.message;
    ASSERT_TRUE (eh::FollowDraftFiles (s, folder.path)) << s.message;
    EXPECT_FALSE (fs::exists (OwnRecovery (s, folder))) << "saved: nothing left to recover";
    EXPECT_FALSE (Tick (s, folder, t2 + std::chrono::seconds (100)));

    // Unsaved again, snapshotted, then back to the saved content: a clean Done deletes it.
    const auto t3 = t2 + std::chrono::seconds (200);
    Draw (s, 40);
    Tick (s, folder, t3);
    ASSERT_TRUE (Tick (s, folder, t3 + std::chrono::seconds (3)));
    ASSERT_TRUE (fs::exists (OwnRecovery (s, folder)));
    ASSERT_TRUE (s.edits.Undo ());
    eh::RequestClose (s);
    ASSERT_TRUE (s.close);
    ASSERT_TRUE (s.draftRequest.has_value ()) << "the host drains the delete before it stops the editor";
    eh::FollowDraftFiles (s, folder.path);
    EXPECT_FALSE (fs::exists (OwnRecovery (s, folder)));
}

// A session that ends without a user close leaves its snapshot; the next session offers it and
// loads nothing unasked; Recover is one undoable, still-unsaved edit; Discard deletes.
TEST (HudVirtualEditorDraft, ANextSessionOffersInterruptedWorkAndRecoverOrDiscardDoWhatTheySay159)
{
    Folder folder;
    ed::Document lost;
    fs::path left;
    {
        eh::Session crashed (Started ());
        Tick (crashed, folder, kT0);
        Draw (crashed, 0);
        Draw (crashed, 10);
        Tick (crashed, folder, kT0 + std::chrono::seconds (1));
        ASSERT_TRUE (Tick (crashed, folder, kT0 + std::chrono::seconds (4)));
        lost = crashed.edits.Doc ();
        left = OwnRecovery (crashed, folder);
    } // forced stop: no close, no prompt
    ASSERT_TRUE (fs::exists (left));
    // An expired snapshot and an unreadable one beside it.
    const fs::path expired = folder.path / "Recovery" / "0000000000000001.tapdraft";
    fs::copy_file (left, expired);
    fs::last_write_time (expired, fs::file_time_type::clock::now () - std::chrono::hours (24 * 15));
    const fs::path broken = folder.path / "Recovery" / "0000000000000002.tapdraft";
    std::ofstream (broken) << "{ not a draft";

    eh::Session next (Started ());
    const auto before = next.edits.Doc ();
    ASSERT_TRUE (Tick (next, folder, kT0));
    ASSERT_EQ (next.recoveries.size (), 1u);
    EXPECT_EQ (next.recoveries[0].file, left.filename ().string ());
    EXPECT_EQ (next.recoveries[0].revision, lost.revision);
    EXPECT_EQ (next.recoveries[0].objects, 2u);
    EXPECT_FALSE (fs::exists (expired)) << "older than the retention";
    EXPECT_TRUE (fs::exists (broken)) << "unreadable work is not deleted, only not offered";
    EXPECT_TRUE (ed::SameAuthoredContent (next.edits.Doc (), before)) << "nothing is loaded unasked";

    ASSERT_TRUE (eh::RequestRecover (next, next.recoveries[0].file)) << next.message;
    ASSERT_TRUE (eh::FollowDraftFiles (next, folder.path)) << next.message;
    EXPECT_TRUE (ed::SameAuthoredContent (next.edits.Doc (), lost));
    EXPECT_EQ (next.edits.Doc ().entities, lost.entities) << "ids restored";
    EXPECT_EQ (next.edits.UndoDepth (), 1u);
    EXPECT_TRUE (eh::DraftDirty (next)) << "recovered work is not a saved draft";
    EXPECT_FALSE (fs::exists (left)) << "this session's snapshots take over";
    EXPECT_TRUE (next.recoveries.empty ());
    EXPECT_NE (next.message.find ("not saved yet"), std::string::npos) << next.message;
    ASSERT_TRUE (next.edits.Undo ());
    EXPECT_TRUE (ed::SameAuthoredContent (next.edits.Doc (), before));

    // Discard: another interrupted session's work is deleted and the document is untouched.
    {
        eh::Session crashed (Started ());
        Tick (crashed, folder, kT0);
        Draw (crashed, 5);
        Tick (crashed, folder, kT0 + std::chrono::seconds (1));
        ASSERT_TRUE (Tick (crashed, folder, kT0 + std::chrono::seconds (4)));
        left = OwnRecovery (crashed, folder);
    }
    eh::Session third (Started ());
    Tick (third, folder, kT0);
    ASSERT_EQ (third.recoveries.size (), 1u);
    ASSERT_TRUE (eh::RequestDiscardRecovery (third, third.recoveries[0].file));
    ASSERT_TRUE (eh::FollowDraftFiles (third, folder.path)) << third.message;
    EXPECT_FALSE (fs::exists (left));
    EXPECT_TRUE (third.recoveries.empty ());
    EXPECT_EQ (third.edits.UndoDepth (), 0u);
    EXPECT_FALSE (eh::RequestRecover (third, "..\\elsewhere.tapdraft")) << "only names inside the folder";
}

// "Keep recovery snapshots" off deletes this session's snapshot and writes no more.
TEST (HudVirtualEditorDraft, TurningRecoveryOffDeletesTheSnapshotAndWritesNoMore159)
{
    Folder folder;
    eh::Session s (Started ());
    Tick (s, folder, kT0);
    Draw (s, 0);
    Tick (s, folder, kT0 + std::chrono::seconds (1));
    ASSERT_TRUE (Tick (s, folder, kT0 + std::chrono::seconds (4)));
    ASSERT_TRUE (fs::exists (OwnRecovery (s, folder)));
    eh::SetRecoveryEnabled (s, false);
    eh::FollowDraftFiles (s, folder.path);
    EXPECT_FALSE (fs::exists (OwnRecovery (s, folder)));
    Draw (s, 10);
    EXPECT_FALSE (Tick (s, folder, kT0 + std::chrono::seconds (30)));
    EXPECT_FALSE (Tick (s, folder, kT0 + std::chrono::seconds (60)));
    EXPECT_FALSE (fs::exists (OwnRecovery (s, folder)));
    eh::SetRecoveryEnabled (s, true);
    ASSERT_TRUE (Tick (s, folder, kT0 + std::chrono::seconds (61))) << "on again: the quiet unsaved work at once";
    EXPECT_EQ (ReadBack (OwnRecovery (s, folder)).entities.size (), 2u);
}
