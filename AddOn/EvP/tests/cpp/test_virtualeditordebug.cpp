#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Geometry/Mesh.hpp"
#include "NodeGraph/Json.hpp"
#include "virtual_editor_debug_replay.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
namespace eh = geomsrv::archviz::edithud;
namespace edit = geomsrv::edit;
namespace lg = tapioca::geometry;
namespace js = evp::nodegraph::json;

struct DebugDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path () /
                                 ("tapioca-debug-export-test-" +
                                  std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()));
    ~DebugDirectory ()
    {
        std::error_code ignored;
        std::filesystem::remove_all (path, ignored);
    }
};

TEST (VirtualEditorDebug, TwoClicksWriteDifferentCompleteFilesWithoutEditingTheScene)
{
    DebugDirectory directory;
    edit::Document doc;
    edit::AddPath (doc, { { -6, 12, 1.2345678901234567 }, { 0, 2, 2 }, { 0, 0, 2 }, { 15, 3, 3 } }, 3);
    std::string error;
    const auto id = doc.paths[0].id;
    ASSERT_TRUE (edit::EditPath (
        doc, id,
        [] (edit::Path& path, std::string&) {
            path.points[1].radius = 7.9; // Preserve a request larger than its fitted short-leg bend.
            path.edgeRadius = 1.2;
            return true;
        },
        error))
        << error;
    eh::Session s (doc);
    const auto before = s.edits.Doc ();
    const auto depth = s.edits.UndoDepth ();
    ASSERT_TRUE (eh::RequestDebugExport (s));
    EXPECT_FALSE (std::filesystem::exists (directory.path)) << "the UI request must not write files";
    ASSERT_TRUE (eh::SaveDebugExport (s, directory.path)) << s.debugExportError;
    const auto first = s.debugExportPath;
    std::ifstream input (std::filesystem::u8path (first), std::ios::binary);
    const std::string text { std::istreambuf_iterator<char> (input), {} };
    const auto parsed = js::Parse (text);
    ASSERT_TRUE (parsed.ok) << parsed.error;
    const auto replay = debugreplay::Read (parsed.value);
    EXPECT_EQ (replay.doc.paths, before.paths);
    EXPECT_EQ (replay.doc.landscape, before.landscape);
    EXPECT_TRUE (parsed.value.Find ("baseline")->IsNull ());
    EXPECT_TRUE (parsed.value.Find ("outcome")->IsNull ());
    bool hadLandscape = true;
    ASSERT_TRUE (parsed.value.Find ("hadLandscape")->AsBool (hadLandscape));
    EXPECT_FALSE (hadLandscape);
    const auto* points = parsed.value.Find ("document")->Find ("paths")->AsArray ()->at (0).Find ("points")->AsArray ();
    double radius = 0, x = 0;
    ASSERT_TRUE (points->at (1).Find ("radius")->AsDouble (radius));
    EXPECT_DOUBLE_EQ (radius, 7.9);
    ASSERT_TRUE (points->at (0).Find ("point")->Find ("z")->AsDouble (x));
    EXPECT_DOUBLE_EQ (x, before.paths[0].points[0].p.z);
    ASSERT_TRUE (eh::RequestDebugExport (s));
    ASSERT_TRUE (eh::SaveDebugExport (s, directory.path)) << s.debugExportError;
    EXPECT_NE (first, s.debugExportPath);
    EXPECT_TRUE (std::filesystem::exists (std::filesystem::u8path (first)));
    EXPECT_EQ (s.edits.Doc ().paths, before.paths);
    EXPECT_EQ (s.edits.Doc ().landscape, before.landscape);
    EXPECT_EQ (s.edits.Doc ().revision, before.revision);
    EXPECT_EQ (s.edits.UndoDepth (), depth);
    EXPECT_EQ (
        std::distance (std::filesystem::directory_iterator (directory.path), std::filesystem::directory_iterator ()),
        2);
}

TEST (VirtualEditorDebug, RefusesUnfinishedInputAndCancelsChangedRevisionWithoutWriting)
{
    DebugDirectory directory;
    eh::Session s;
    s.drawing.push_back ({ 1, 2, 3 });
    EXPECT_FALSE (eh::RequestDebugExport (s));
    s.drawing.clear ();
    ASSERT_TRUE (eh::RequestDebugExport (s));
    ASSERT_TRUE (s.edits.Apply ([] (edit::Document& doc, std::string&) {
        edit::AddPath (doc, { { 0, 0, 0 }, { 2, 0, 0 } }, 1);
        return true;
    }));
    EXPECT_FALSE (eh::SaveDebugExport (s, directory.path));
    EXPECT_FALSE (std::filesystem::exists (directory.path));
    EXPECT_FALSE (s.debugExportError.empty ());
}

TEST (VirtualEditorDebug, WriteFailureIsVisibleAndDoesNotClaimSuccess)
{
    DebugDirectory directory;
    std::filesystem::create_directories (directory.path);
    const auto blocker = directory.path / "not-a-directory";
    {
        std::ofstream file (blocker);
        file << "keep";
    }
    eh::Session s;
    ASSERT_TRUE (eh::RequestDebugExport (s));
    EXPECT_FALSE (eh::SaveDebugExport (s, blocker));
    EXPECT_TRUE (s.debugExportPath.empty ());
    EXPECT_FALSE (s.debugExportError.empty ());
    EXPECT_FALSE (s.debugExportRequested);
    std::ifstream file (blocker);
    std::string text;
    file >> text;
    EXPECT_EQ (text, "keep");
}

TEST (VirtualEditorDebug, BrokenLandscapeStillExportsInsteadOfRunningPersistenceValidation)
{
    edit::Document doc;
    doc.landscape.emplace ();
    doc.landscape->propertyLineSourceId = 1234; // Deliberately orphaned.
    eh::Session s (doc);
    std::string text, error;
    ASSERT_TRUE (eh::EncodeDebugScene (s, text, error)) << error;
    const auto parsed = js::Parse (text);
    ASSERT_TRUE (parsed.ok) << parsed.error;
    std::string property;
    ASSERT_TRUE (parsed.value.Find ("document")->Find ("landscape")->Find ("propertyLine")->AsString (property));
    EXPECT_EQ (property, "1234");
}
} // namespace
