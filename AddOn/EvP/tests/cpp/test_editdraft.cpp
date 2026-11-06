// Durable draft envelope (Edit/EditDraft.hpp, #159 D1): the whole authored document round-trips
// through the virtual codec with its ids; every malformed, foreign or newer file is refused with
// the caller's document untouched.
#include "Edit/EditDraft.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;
namespace geo = tapioca::geometry;

namespace {
ed::Document Authored ()
{
    ed::Document doc;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (doc, { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6 }, error)) << error;
    const ed::Id a = ed::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, true);
    const ed::Id b = ed::AddPolyline (doc, { { 20, 0, 0 }, { 24, 0, 0 }, { 24, 4, 0 } }, true);
    EXPECT_TRUE (geo::SetLandscapeSourceRole (doc, a, geo::SourceRole::ReferenceLine, error)) << error;
    EXPECT_TRUE (geo::SetLandscapeSourceRole (doc, b, geo::SourceRole::RegionBoundary, error)) << error;
    ed::AddPath (doc, { { 0, -5, 0 }, { 30, -5, 0 } }, 3.0);
    doc.revision = 41;
    return doc;
}
} // namespace

TEST (EditDraft, TheWholeAuthoredDocumentRoundTripsWithItsIds)
{
    const ed::Document doc = Authored ();
    std::string text, error;
    ASSERT_TRUE (geo::WriteDraft (doc, "2026-11-06T12:00:00Z", text, error)) << error;
    ed::Document loaded;
    geo::DraftInfo info;
    ASSERT_TRUE (geo::ReadDraft (text, loaded, info, error)) << error;
    EXPECT_TRUE (ed::SameAuthoredContent (loaded, doc));
    EXPECT_EQ (loaded.entities, doc.entities);
    EXPECT_EQ (loaded.paths, doc.paths);
    EXPECT_EQ (loaded.landscape, doc.landscape);
    EXPECT_GE (loaded.nextId, doc.nextId) << "ids handed out must never be handed out again";
    EXPECT_EQ (info.version, geo::kDraftVersion);
    EXPECT_EQ (info.savedUtc, "2026-11-06T12:00:00Z");
    EXPECT_EQ (info.revision, 41u);
    EXPECT_EQ (info.session, "09080706050403020100010203040506");
    // Deterministic for a fixed clock: the same document writes the same file.
    std::string again;
    ASSERT_TRUE (geo::WriteDraft (doc, "2026-11-06T12:00:00Z", again, error));
    EXPECT_EQ (again, text);
}

TEST (EditDraft, ADocumentWithoutALandscapeIsNotADraft)
{
    ed::Document doc;
    ed::AddPolyline (doc, { { 0, 0, 0 }, { 1, 0, 0 } }, false);
    std::string text, error;
    EXPECT_FALSE (geo::WriteDraft (doc, "t", text, error));
    EXPECT_FALSE (error.empty ());
}

TEST (EditDraft, ForeignNewerOrBrokenFilesLeaveTheDocumentAlone)
{
    std::string good, error;
    ASSERT_TRUE (geo::WriteDraft (Authored (), "t", good, error)) << error;
    const auto replace = [&] (const std::string& from, const std::string& to) {
        std::string text = good;
        const auto at = text.find (from);
        EXPECT_NE (at, std::string::npos) << from;
        return at == std::string::npos ? text : text.replace (at, from.size (), to);
    };
    const std::pair<std::string, const char*> cases[] = {
        { "not json at all", "not valid JSON" },
        { "{}", "not a Tapioca" },
        { replace ("tapioca.virtual-editor.draft", "something.else"), "not a Tapioca" },
        { replace ("\"version\": 1", "\"version\": 2"), "newer" },
        { replace ("\"version\": 1", "\"version\": 0"), "version" },
        { replace ("\"definition\"", "\"defined\""), "incomplete" },
        { replace ("\"definition\": \"{", "\"definition\": \"{broken"), "definition" },
    };
    for (const auto& [text, reason] : cases) {
        ed::Document doc = Authored ();
        const ed::Document before = doc;
        geo::DraftInfo info;
        info.savedUtc = "kept";
        std::string why;
        EXPECT_FALSE (geo::ReadDraft (text, doc, info, why)) << text.substr (0, 60);
        EXPECT_NE (why.find (reason), std::string::npos) << why;
        EXPECT_EQ (doc.entities, before.entities);
        EXPECT_EQ (doc.landscape, before.landscape);
        EXPECT_EQ (info.savedUtc, "kept");
    }
}

TEST (EditDraft, FileNamesStayInsideTheDraftsFolder)
{
    EXPECT_EQ (geo::DraftFileName ("Site A"), "Site A.tapdraft");
    EXPECT_EQ (geo::DraftFileName ("Site A.tapdraft"), "Site A.tapdraft");
    EXPECT_EQ (geo::DraftFileName ("..\\..\\Windows\\x"), "Windows_x.tapdraft");
    EXPECT_EQ (geo::DraftFileName ("a/b:c*?"), "a_b_c__.tapdraft");
    EXPECT_EQ (geo::DraftFileName ("  ..  "), "");
    EXPECT_EQ (geo::DraftFileName (""), "");
    EXPECT_EQ (geo::DraftFileName (std::string (200, 'x')).size (), 80u + std::string (".tapdraft").size ());
}
