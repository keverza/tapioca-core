#include "Grasshopper/Gh2OverlayRequest.hpp"
#include <gtest/gtest.h>

using namespace evp::grasshopper;

TEST (Gh2OverlayRequest, OwnGenerationOnly)
{
    const std::string layer = "gh2.12.0123456789abcdef0123456789abcdef";
    EXPECT_TRUE (IsGh2OverlayLayer (layer, 12));
    EXPECT_FALSE (IsGh2OverlayLayer (layer, 13));
    EXPECT_FALSE (IsGh2OverlayLayer (layer + "0", 12));
    EXPECT_FALSE (IsGh2OverlayLayer ("gh2.12.not-a-guid", 12));
    EXPECT_FALSE (IsGh2OverlayLayer ("gh2.0.0123456789abcdef0123456789abcdef", 0));
}

TEST (Gh2OverlayRequest, SeparateViewsAndNoGlobalClearOrOff)
{
    const std::string start = "{\"layer\":\"gh2.12.0123456789abcdef0123456789abcdef\"";
    EXPECT_TRUE (IsGh2OverlayRequest ("Tapioca.SetOverlayLayer", start + ",\"views\":\"2d\",\"polylines\":[]}", 12));
    EXPECT_TRUE (IsGh2OverlayRequest ("Tapioca.SetOverlayLayer", start + ",\"views\":\"3d\",\"meshes\":[]}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.SetOverlayLayer", start + ",\"views\":\"both\",\"meshes\":[]}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.SetOverlayLayer", start + ",\"views\":\"3d\",\"meshes\":[]}", 13));
    EXPECT_TRUE (IsGh2OverlayRequest ("Tapioca.ClearOverlayLayer", start + "}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.ClearOverlayLayer", start + ",\"all\":true}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.ClearOverlayLayer", "{\"all\":true}", 12));
    EXPECT_TRUE (IsGh2OverlayRequest ("Tapioca.Overlay2D", "{\"action\":\"on\"}", 12));
    EXPECT_TRUE (IsGh2OverlayRequest ("Tapioca.Overlay3D", "{\"action\":\"on\"}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.Overlay3D", "{\"action\":\"off\"}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.DeleteElements", start + "}", 12));
    EXPECT_FALSE (
        IsGh2OverlayRequest ("Tapioca.SetOverlayLayer",
                             start + ",\"views\":\"3d\",\"notes\":\"" + std::string (1024 * 1024, 'x') + "\"}", 12));
}
