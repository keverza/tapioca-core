// What the HUD's tests share: the bundled font, a fresh engine laid out for a pointer on a
// 1200 x 800 view, and a click as the input layer hands it over.

#ifndef EVP_TESTS_HUD_FIXTURE_HPP
#define EVP_TESTS_HUD_FIXTURE_HPP

#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayLayers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace hudtest {

namespace hud = geomsrv::archviz::overlayhud;
namespace layers = geomsrv::archviz::overlaylayers;

inline std::vector<uint8_t> Font ()
{
    std::ifstream stream (EVP_SCENE_TEXT_FONT, std::ios::binary);
    return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char> () };
}

inline layers::PanelItem Item (layers::ItemKind kind, const std::string& text = "", const std::string& value = "")
{
    layers::PanelItem item;
    item.kind = kind;
    item.text = text;
    item.value = value;
    return item;
}

inline hud::Input At (float x, float y, std::vector<hud::Input::Button> buttons = {})
{
    hud::Input input;
    input.width = 1200.0f;
    input.height = 800.0f;
    input.pointer = true;
    input.x = x;
    input.y = y;
    input.buttons = std::move (buttons);
    return input;
}

// A fresh engine per test: what one test folded or set must not reach the next one's.
struct Fresh {
    hud::Engine engine;
    Fresh ()
    {
        std::string error;
        EXPECT_TRUE (engine.Init (Font (), error)) << error;
    }
    std::vector<std::string> Keys (size_t count) const
    {
        std::vector<std::string> keys;
        for (size_t i = 0; i < count; ++i)
            keys.push_back ("hud#" + std::to_string (i));
        return keys;
    }
    hud::Layout Lay (const std::vector<const layers::Panel*>& panels, const hud::Input& input)
    {
        hud::Layout out;
        std::string error;
        EXPECT_TRUE (engine.Build (panels, Keys (panels.size ()), 1.0f, input, {}, out, error)) << error;
        return out;
    }
    // A click where the pointer is, as the input layer hands it over: the pointer there,
    // a press, then a release.
    hud::Layout Click (const std::vector<const layers::Panel*>& panels, float x, float y)
    {
        Lay (panels, At (x, y));
        Lay (panels, At (x, y, { { 0, true } }));
        return Lay (panels, At (x, y, { { 0, false } }));
    }
};

// An engine that keeps what the user did in a state the test can read, and hears every
// change.
struct Watched : Fresh {
    std::shared_ptr<hud::State> state = hud::NewState ();
    std::vector<hud::Change> heard;
    Watched ()
    {
        engine.UseState (state);
        engine.SetChangeSink ([this] (const hud::Change& change) { heard.push_back (change); });
    }
    double Value (const std::string& id) const
    {
        for (const auto& held : hud::Values (*state, "hud#0"))
            if (held.first == id)
                return held.second;
        return -1.0;
    }
};

// Whether any of a set's triangles is in `rgba`, the panels' and what floats over them.
inline bool Drawn (const hud::Layout& layout, uint32_t rgba)
{
    for (const hud::Built& built : layout.panels)
        for (const hud::Vertex& v : built.vertices)
            if (v.rgba == rgba)
                return true;
    for (const hud::Vertex& v : layout.overlay.vertices)
        if (v.rgba == rgba)
            return true;
    return false;
}

// Where a colour is drawn in one Built: the box of its vertices, from its top-left.
inline bool Box (const hud::Built& built, uint32_t rgba, float box[4])
{
    box[0] = box[1] = FLT_MAX;
    box[2] = box[3] = -FLT_MAX;
    for (const hud::Vertex& v : built.vertices)
        if (v.rgba == rgba) {
            box[0] = (std::min) (box[0], v.x);
            box[1] = (std::min) (box[1], v.y);
            box[2] = (std::max) (box[2], v.x);
            box[3] = (std::max) (box[3], v.y);
        }
    return box[0] <= box[2];
}

} // namespace hudtest

#endif
