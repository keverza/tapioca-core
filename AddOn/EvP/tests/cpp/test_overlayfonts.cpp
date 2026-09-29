// ArchViz/OverlayFonts: a font a caller names -- an installed family by the name
// Windows lists it under, or a font file -- found once, as the file every engine keys
// on; and a text in a font of its own laid out by that font's engine, its atlas pages
// composed with the bundled font's.

#include "ArchViz/OverlayFonts.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayScene.hpp"
#include "ArchViz/OverlayText.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace fonts = geomsrv::archviz::overlayfonts;
namespace hud = geomsrv::archviz::overlayhud;
namespace layers = geomsrv::archviz::overlaylayers;
namespace scene = geomsrv::archviz::overlayscene;
namespace text = geomsrv::archviz::overlaytext;

namespace {

std::vector<uint8_t> Bundled ()
{
    std::ifstream stream (EVP_SCENE_TEXT_FONT, std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
}

// Arial: on every Windows this suite runs on.
std::string Arial ()
{
    std::string path, error;
    EXPECT_TRUE (fonts::Resolve ("Arial", path, error)) << error;
    return path;
}

} // namespace

TEST (OverlayFonts, ARegistryNameListsItsFacesWhateverTheCase)
{
    EXPECT_TRUE (fonts::Lists ("Arial (TrueType)", "Arial"));
    EXPECT_TRUE (fonts::Lists ("Arial (TrueType)", " arial "));
    EXPECT_FALSE (fonts::Lists ("Arial Bold (TrueType)", "Arial"));
    EXPECT_TRUE (fonts::Lists ("Arial Bold (TrueType)", "Arial Bold"));
    EXPECT_TRUE (fonts::Lists ("Cambria & Cambria Math (TrueType)", "Cambria Math"));
    EXPECT_TRUE (fonts::Lists ("Cambria & Cambria Math (TrueType)", "cambria"));
    EXPECT_TRUE (fonts::Lists ("Source Code Pro (OpenType)", "Source Code Pro"));
    EXPECT_TRUE (fonts::Lists ("My Font", "My Font")); // a user's font, listed bare
    EXPECT_FALSE (fonts::Lists ("Arial (TrueType)", ""));
}

TEST (OverlayFonts, AnInstalledFamilyAndAFilePathResolveAndAnUnknownOneSaysSo)
{
    const std::string arial = Arial ();
    ASSERT_FALSE (arial.empty ());
    std::string again, error;
    EXPECT_TRUE (fonts::Resolve (arial, again, error)) << error; // a path is taken as it is
    EXPECT_EQ (again, arial);
    std::vector<uint8_t> bytes;
    EXPECT_TRUE (fonts::Read (arial, bytes, error)) << error;
    EXPECT_GT (bytes.size (), 1000u);
    std::string none;
    EXPECT_FALSE (fonts::Resolve ("No Such Font Tapioca 7", none, error));
    EXPECT_NE (error.find ("is installed"), std::string::npos) << error;
    EXPECT_TRUE (none.empty ());
}

// Two texts in two fonts: each laid out by its own font's engine, the pages both use
// composed into one list, every glyph's page within it.
TEST (OverlayFonts, ATextInItsOwnFontUsesThatFontsPages)
{
    static text::Engine bundled;
    std::string error;
    // The small seed: the full one costs seconds in a debug build, and the test needs neither.
    ASSERT_TRUE (bundled.Ready () || bundled.Init (Bundled (), error, text::Engine::SmallSeedText ())) << error;
    static text::Engine arial;
    std::vector<uint8_t> bytes;
    ASSERT_TRUE (fonts::Read (Arial (), bytes, error)) << error;
    ASSERT_TRUE (arial.Ready () || arial.Init (std::move (bytes), error, text::Engine::SmallSeedText ())) << error;
    const std::string arialPath = Arial ();
    const scene::FontResolver resolver = [&] (const std::string& font) { return font == arialPath ? &arial : nullptr; };

    layers::Layer layer;
    layer.name = "fonts";
    layers::Text plain;
    plain.text = "Bundled";
    plain.screen = true;
    layers::Text own = plain;
    own.text = "Arial Qq";
    own.font = arialPath;
    layer.texts = { plain, own };
    const scene::Scene drawn =
        scene::PrepareScene ({ std::make_shared<const layers::Layer> (layer) }, &bundled, nullptr, 1.0f, resolver);
    EXPECT_EQ (drawn.problems.textsNotLaidOut, 0u) << drawn.problems.lastError;
    std::set<uint64_t> ids;
    for (const auto& page : drawn.pages)
        ids.insert (page->id);
    bool fromArial = false, fromBundled = false;
    for (const auto& page : arial.Pages ())
        fromArial = fromArial || ids.count (page->id) != 0;
    for (const auto& page : bundled.Pages ())
        fromBundled = fromBundled || ids.count (page->id) != 0;
    EXPECT_TRUE (fromArial);
    EXPECT_TRUE (fromBundled);
    for (const scene::GlyphDraw& draw : drawn.glyphDraws)
        EXPECT_LT (draw.page, drawn.pages.size ());

    // A font the resolver cannot give is drawn in the bundled font, and says why.
    layer.texts[1].font = "C:/no/such.ttf";
    const scene::Scene fallback =
        scene::PrepareScene ({ std::make_shared<const layers::Layer> (layer) }, &bundled, nullptr, 1.0f, resolver);
    EXPECT_EQ (fallback.problems.textsNotLaidOut, 0u);
    EXPECT_NE (fallback.problems.lastError.find ("bundled font"), std::string::npos);
    EXPECT_FALSE (fallback.glyphs.empty ());
}

// A HUD panel in a font of its own: ImGui takes the font into its atlas once.
TEST (OverlayFonts, AHudPanelLoadsItsFontOnce)
{
    hud::Engine engine;
    std::string error;
    ASSERT_TRUE (engine.Init (Bundled (), error)) << error;
    engine.SetFontLoader (&fonts::Read);
    layers::Panel panel;
    panel.title = "Arial panel";
    panel.font = Arial ();
    layers::PanelItem row;
    row.kind = layers::ItemKind::Row;
    row.text = "GFA";
    row.value = "2633 m2";
    panel.items = { row };
    std::vector<hud::Built> built;
    ASSERT_TRUE (engine.Build ({ &panel }, 1.0f, built, error)) << error;
    ASSERT_TRUE (engine.Build ({ &panel }, 1.0f, built, error)) << error;
    EXPECT_EQ (engine.GetStats ().fonts, 2u);
    ASSERT_EQ (built.size (), 1u);
    EXPECT_FALSE (built[0].vertices.empty ());

    panel.font = "C:/no/such.ttf";
    EXPECT_FALSE (engine.Build ({ &panel }, 1.0f, built, error));
    EXPECT_NE (error.find ("bundled font"), std::string::npos) << error;
    EXPECT_FALSE (built[0].vertices.empty ()); // drawn anyway
}
