// ArchViz/ClayLook -- the clay material override (issue #14 P2).
//
// The override is invisible when it is wrong in the subtle direction: a pane
// that stays in the blended pass while drawn opaque, or a metal that keeps its
// metallic F0 and turns clay into chrome, both render a plausible image. So the
// resolver's contract is pinned here, offline, rather than judged by eye.

#include "ArchViz/ClayLook.hpp"

#include <gtest/gtest.h>

using namespace geomsrv::archviz;

namespace {

SurfaceMaterial Paint ()
{
    SurfaceMaterial m;
    m.r = 0.6f;
    m.g = 0.1f;
    m.b = 0.05f;
    m.alpha = 1.0f;
    return m;
}

SurfaceMaterial Glass ()
{
    SurfaceMaterial m;
    m.r = 0.2f;
    m.g = 0.5f;
    m.b = 0.4f;
    m.alpha = 0.3f;
    return m;
}

SurfacePreset Metal ()
{
    SurfacePreset p;
    p.metallic = 1.0f;
    p.roughness = 0.2f;
    p.reflectance = 0.9f;
    return p;
}

} // namespace

TEST (ClayLook, OffPassesTheMaterialAndPresetThrough)
{
    const SurfaceMaterial m = Paint ();
    const SurfacePreset p = Metal ();
    const DrawnSurface d = ResolveDrawnSurface (m, p, ClayLook {});
    EXPECT_FLOAT_EQ (d.r, m.r);
    EXPECT_FLOAT_EQ (d.g, m.g);
    EXPECT_FLOAT_EQ (d.b, m.b);
    EXPECT_FLOAT_EQ (d.alpha, m.alpha);
    EXPECT_FLOAT_EQ (d.roughness, p.roughness);
    EXPECT_FLOAT_EQ (d.reflectance, p.reflectance);
    EXPECT_FLOAT_EQ (d.metallic, p.metallic);
}

TEST (ClayLook, OpaqueSurfaceBecomesNeutralDielectricClay)
{
    ClayLook clay;
    clay.enabled = true;
    clay.roughness = 0.7f;
    const DrawnSurface d = ResolveDrawnSurface (Paint (), Metal (), clay);
    EXPECT_FLOAT_EQ (d.r, clay.r);
    EXPECT_FLOAT_EQ (d.g, clay.g);
    EXPECT_FLOAT_EQ (d.b, clay.b);
    EXPECT_FLOAT_EQ (d.alpha, 1.0f);
    EXPECT_FLOAT_EQ (d.roughness, 0.7f);
    EXPECT_FLOAT_EQ (d.reflectance, 0.5f);
    // A metallic F0 is the base colour: kept, it would draw clay as chrome.
    EXPECT_FLOAT_EQ (d.metallic, 0.0f);
}

TEST (ClayLook, KeptGlassKeepsOpacityAndFinishButLosesTint)
{
    ClayLook clay;
    clay.enabled = true;
    SurfacePreset glassPreset;
    glassPreset.roughness = 0.05f;
    glassPreset.reflectance = 0.6f;
    glassPreset.metallic = 1.0f; // a misclassified pane must still not turn to chrome
    const SurfaceMaterial g = Glass ();
    const DrawnSurface d = ResolveDrawnSurface (g, glassPreset, clay);
    EXPECT_FLOAT_EQ (d.r, clay.r);
    EXPECT_FLOAT_EQ (d.alpha, g.alpha);
    EXPECT_LT (d.alpha, kOpaqueAlpha);
    EXPECT_FLOAT_EQ (d.roughness, 0.05f);
    EXPECT_FLOAT_EQ (d.reflectance, 0.6f);
    EXPECT_FLOAT_EQ (d.metallic, 0.0f);
}

TEST (ClayLook, DroppedGlassBecomesOpaqueAndLeavesTheBlendedPass)
{
    ClayLook clay;
    clay.enabled = true;
    clay.keepGlass = false;
    const DrawnSurface d = ResolveDrawnSurface (Glass (), SurfacePreset {}, clay);
    EXPECT_FLOAT_EQ (d.alpha, 1.0f);
    EXPECT_GE (d.alpha, kOpaqueAlpha);
    EXPECT_FLOAT_EQ (d.roughness, clay.roughness);
}

// Every opaque/translucent partition (shadow casters, the AO depth prepass) reads
// DrawnAlpha; it must agree with the alpha the image is drawn with.
TEST (ClayLook, DrawnAlphaAgreesWithTheResolvedSurface)
{
    for (const bool enabled : { false, true })
        for (const bool keepGlass : { false, true })
            for (const SurfaceMaterial& m : { Paint (), Glass () }) {
                ClayLook clay;
                clay.enabled = enabled;
                clay.keepGlass = keepGlass;
                EXPECT_FLOAT_EQ (DrawnAlpha (m, clay), ResolveDrawnSurface (m, SurfacePreset {}, clay).alpha)
                    << "enabled=" << enabled << " keepGlass=" << keepGlass << " alpha=" << m.alpha;
            }
}

TEST (ClayLook, HudColourIsDecodedFromSrgbToLinear)
{
    // The HUD default, 0xE6E3DE, must reproduce ClayLook's linear default.
    const ClayLook fromHud = ClayLookFromHud (true, 0xE6E3DEFFu, 0.8f, true);
    const ClayLook defaults;
    EXPECT_NEAR (fromHud.r, defaults.r, 1.0e-3f);
    EXPECT_NEAR (fromHud.g, defaults.g, 1.0e-3f);
    EXPECT_NEAR (fromHud.b, defaults.b, 1.0e-3f);
    EXPECT_TRUE (fromHud.enabled);
    EXPECT_TRUE (fromHud.keepGlass);

    // Mid grey sRGB 0x80 is linear ~0.216, not 0.5: the decode is not skipped.
    const ClayLook grey = ClayLookFromHud (true, 0x808080FFu, 2.0f, false);
    EXPECT_NEAR (grey.r, 0.2158f, 1.0e-3f);
    EXPECT_FLOAT_EQ (grey.roughness, 1.0f); // clamped
    EXPECT_FALSE (grey.keepGlass);
}

TEST (ClayLook, LuminanceIsRec709)
{
    ClayLook clay;
    clay.r = clay.g = clay.b = 0.5f;
    EXPECT_NEAR (ClayLuminance (clay), 0.5f, 1.0e-6f);
    clay.r = 1.0f;
    clay.g = clay.b = 0.0f;
    EXPECT_NEAR (ClayLuminance (clay), 0.2126f, 1.0e-6f);
}

// #230: the sun study's display model, now resolved through ClayLook in every pass,
// must draw exactly what the forward pass's old ternaries drew -- white, roughness 1,
// specular 0.5, dielectric, and glass OPAQUE so it casts, occludes and leaves the
// blended pass.
TEST (ClayLook, StudyModelMatchesTheOldForwardOverride)
{
    const ClayLook study = StudyModelLook ();
    for (const SurfaceMaterial& m : { Paint (), Glass () }) {
        const DrawnSurface d = ResolveDrawnSurface (m, Metal (), study);
        EXPECT_FLOAT_EQ (d.r, 1.0f);
        EXPECT_FLOAT_EQ (d.g, 1.0f);
        EXPECT_FLOAT_EQ (d.b, 1.0f);
        EXPECT_FLOAT_EQ (d.alpha, 1.0f);
        EXPECT_FLOAT_EQ (d.roughness, 1.0f);
        EXPECT_FLOAT_EQ (d.reflectance, 0.5f);
        EXPECT_FLOAT_EQ (d.metallic, 0.0f);
        EXPECT_FLOAT_EQ (DrawnAlpha (m, study), 1.0f);
    }
}
