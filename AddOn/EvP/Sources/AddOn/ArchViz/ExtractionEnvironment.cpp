// ArchViz/ExtractionEnvironment — the surface pool and the sun. See the header
// for why these two live apart from the slicing machinery in
// ExtractionThread.cpp.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ExtractionEnvironment.hpp"

#include "ProjectEnv/ProjectSun.hpp"

#include "ArchViz/ArchVizLog.hpp" // ArchVizLog
#include "ArchViz/ColourSpace.hpp"
#include "ArchViz/MaterialTable.hpp"
#include "ArchViz/SceneCmdQueue.hpp" // EnvironmentUpload

#include <AttributeIndex.hpp>
#include <Model.hpp>
#include <ModelMaterial.hpp>
#include <Texture.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

namespace {

// ⚠️ THE SUN IS READ ON EVERY ENVIRONMENT-ONLY POLL, AND
// LOGGING EVERY READING BURIED THE LOG. `ModelWatch`'s environment-only branch
// already carries the rule -- "COUNTED, NOT LOGGED ... a line each would bury
// the events that matter in the one log the whole viewer shares" -- and it
// states that at the CALL SITE while this callee wrote three or four lines
// unconditionally. Archicad raises `isEnvironmentChanged` whenever the 3D
// window's projection moves, which is every time the user navigates, so the
// watch arrived here about 1.2 times a second for as long as the overlay ran.
//
// ⚠️ THE MEASUREMENT, BECAUSE "IT IS A BIT CHATTY" WOULD
// NOT HAVE JUSTIFIED TOUCHING IT. The 2026-09-18 log holds 2007 of these blocks
// and exactly ONE distinct value between them: 6018 byte-identical lines, 36% of
// everything the viewer recorded that day, and 62% of the lines written during
// the menu-driven overlay session inside it. Each line is two data-directory
// resolutions, a directory-chain creation and an open/append/close
// (ArchVizLog.cpp) on the MAIN thread -- the thread the user is navigating with,
// and the one the "lower performance than before" report is about.
//
// ⚠️ AND THE SUPPRESSION SAYS HOW MUCH IT SUPPRESSED.
// Section 7 of OVERLAY-INVARIANTS.md: nothing on this path declines silently.
// The count rides on the next line actually written, so a reader can always tell
// a sun that was read once from a sun that was read two thousand times.
std::string g_lastEnvironmentLog;
uint64_t g_environmentReadsSuppressed = 0;

// The block, written only when it differs from the block before it.
//
// ⚠️ THE KEY IS THE TEXT ITSELF, not a field-by-field
// comparison of `EnvironmentUpload`. A comparison has to be kept in step with
// the struct and silently stops noticing whatever field is added next; the
// rendered text changes if and only if something that gets LOGGED changed, which
// is the exact question being asked.
void SayEnvironmentOnChange (const std::vector<std::string>& lines)
{
    std::string joined;
    for (const std::string& line : lines) {
        joined += line;
        joined += '\n';
    }
    if (joined == g_lastEnvironmentLog) {
        ++g_environmentReadsSuppressed;
        return;
    }

    g_lastEnvironmentLog = joined;
    for (size_t i = 0; i < lines.size (); ++i) {
        if (i == 0 && g_environmentReadsSuppressed > 0)
            ArchVizLog (lines[i] + "  |  the previous reading was unchanged across " +
                        std::to_string (g_environmentReadsSuppressed) + " further reads, not logged");
        else
            ArchVizLog (lines[i]);
    }
    g_environmentReadsSuppressed = 0;
}

} // namespace

void ForgetEnvironmentLog ()
{
    g_lastEnvironmentLog.clear ();
    g_environmentReadsSuppressed = 0;
}

namespace {

// #252: a material's texture -> SurfaceMaterial's texture fields, and the image into `table`.
// MAIN THREAD (ModelerAPI). Images are read once per Archicad fingerprint and kept for the session:
// a live sync re-reads the materials every time the model changes, and re-reading a 4K image
// through GetPixelMap each time would stall the main thread for nothing.
void ReadMaterialTexture (const ModelerAPI::Model& model, const ModelerAPI::Material& material, SurfaceMaterial& m,
                          MaterialTable& table)
{
    static std::map<std::string, TexturePixelsPtr> s_images; // main thread only
    try {
        ModelerAPI::AttributeIndex textureIndex;
        material.GetTextureIndex (textureIndex);
        if (textureIndex.GetIndex () <= 0)
            return;
        ModelerAPI::Texture texture;
        model.GetTexture (textureIndex, &texture);
        // ⚠️ NOT GATED ON IsBumpMapPattern. The pattern flags are independent: "bump mapping" says
        // the image is ALSO used as a bump map, and Archicad's 3D window still draws it as the
        // surface's colour. Measured on SimpleArchicadScene (Material Texture Check, 2026-11-19):
        // every texture the model used carried the flag, and skipping them left none drawn.
        if (!texture.IsAvailable ())
            return;
        const Int32 width = texture.GetPixelMapXSize ();
        const Int32 height = texture.GetPixelMapYSize ();
        if (width <= 0 || height <= 0)
            return;
        const std::string identity = texture.GetFingerprint ().ToCStr ().Get ();
        const std::string key = identity.empty () ? std::string (texture.GetName ().ToCStr ().Get ()) : identity;
        auto found = s_images.find (key);
        if (found == s_images.end ()) {
            auto pixels = std::make_shared<TexturePixels> ();
            pixels->name = texture.GetName ().ToCStr ().Get ();
            pixels->width = uint32_t (width);
            pixels->height = uint32_t (height);
            pixels->rgba.resize (size_t (width) * size_t (height) * 4);
            if (texture.GetPixelType () == ModelerAPI::Texture::ARGBPixelType) {
                std::vector<ModelerAPI::Texture::ARGBPixel> argb (size_t (width) * size_t (height));
                texture.GetPixelMap (argb.data ());
                for (size_t i = 0; i < argb.size (); ++i) {
                    pixels->rgba[i * 4 + 0] = argb[i].red;
                    pixels->rgba[i * 4 + 1] = argb[i].green;
                    pixels->rgba[i * 4 + 2] = argb[i].blue;
                    pixels->rgba[i * 4 + 3] = texture.HasAlphaChannel () ? argb[i].alpha : 255;
                    pixels->hasAlpha = pixels->hasAlpha || pixels->rgba[i * 4 + 3] < 255;
                }
            }
            else {
                // Grey and black-and-white maps: one call per pixel through the colour getter,
                // which every pixel type answers. Rare, and small.
                for (Int32 y = 0; y < height; ++y)
                    for (Int32 x = 0; x < width; ++x) {
                        ModelerAPI::Color c;
                        texture.GetPixelColor (x, y, &c);
                        uint8_t* px = &pixels->rgba[(size_t (y) * size_t (width) + size_t (x)) * 4];
                        px[0] = uint8_t (std::clamp (c.red, 0.0, 1.0) * 255.0 + 0.5);
                        px[1] = uint8_t (std::clamp (c.green, 0.0, 1.0) * 255.0 + 0.5);
                        px[2] = uint8_t (std::clamp (c.blue, 0.0, 1.0) * 255.0 + 0.5);
                        px[3] = 255;
                    }
            }
            FitTexturePixels (*pixels, kMaxTextureSide);
            if (s_images.size () > 512)
                s_images.clear (); // a session that cycled through a whole library: start over
            found = s_images.emplace (key, std::move (pixels)).first;
            ArchVizLog ("Diligent textures: read '" + found->second->name + "' " + std::to_string (width) + "x" +
                        std::to_string (height) + " -> " + std::to_string (found->second->width) + "x" +
                        std::to_string (found->second->height) + ", " + std::to_string (texture.GetXSize ()) + " x " +
                        std::to_string (texture.GetYSize ()) + " per repeat");
        }
        m.textureKey = TextureKeyOf (key);
        m.textureSizeX = texture.GetXSize () > 1e-6 ? float (texture.GetXSize ()) : 1.0f;
        m.textureSizeY = texture.GetYSize () > 1e-6 ? float (texture.GetYSize ()) : 1.0f;
        m.textureMirrorX = texture.IsMirroredInX ();
        m.textureMirrorY = texture.IsMirroredInY ();
        const double angle = material.GetTextureRotationAngle ();
        m.textureRotation = TextureRotationOf (angle);
        // Live check of the rotation's convention: one line per rotated material and angle.
        static std::map<std::string, double> s_rotationsLogged; // main thread only
        if (m.textureRotation.sinA != 0.0f) {
            const std::string materialName = material.GetName ().ToCStr ().Get ();
            auto logged = s_rotationsLogged.find (materialName);
            if (logged == s_rotationsLogged.end () || logged->second != angle) {
                s_rotationsLogged[materialName] = angle;
                ArchVizLog ("Diligent textures: '" + materialName + "' rotates '" + found->second->name + "' by " +
                            std::to_string (angle * 57.29577951308232) + " deg");
            }
        }
        table.SetTexture (m.textureKey, found->second);
    }
    catch (const GS::Exception&) {
        m.textureKey = 0; // an unreadable texture draws the surface colour, as before #252
    }
}

} // namespace

// The model's surface pool -> MaterialTable. MAIN THREAD (it is ModelerAPI).
//
// ⚠️ 1-BASED, AND THE INDEX IS THE POOL'S, NOT ARCHICAD'S ATTRIBUTE INDEX. The
// numbers here are what `Mesh::triMaterial` carries — `AttributeIndex::GetIndex`
// on the polygon's material — so they line up with MaterialRange::material by
// construction. Using an Archicad attribute index would produce a table that
// looks right and matches nothing (ModelAppearanceCommands.cpp's warning).
std::unique_ptr<MaterialTable> ReadMaterials (const ModelerAPI::Model& model)
{
    auto table = std::make_unique<MaterialTable> ();

    const Int32 count = model.GetMaterialCount ();
    for (Int32 i = 1; i <= count; ++i) {
        const ModelerAPI::AttributeIndex index (ModelerAPI::AttributeIndex::MaterialIndex, i);
        ModelerAPI::Material material;
        model.GetMaterial (index, &material);

        SurfaceMaterial m;
        m.index = static_cast<int32_t> (i);

        // ⚠️ sRGB IN, LINEAR OUT, AND THE DECODE IS NOT OPTIONAL. Archicad's
        // surface colour is DISPLAY-REFERRED: the project's RAL paints carry
        // exactly their published sRGB values (RAL 7016 arrives 0.220/0.243/
        // 0.259, whose linear reflectance is 0.040/0.048/0.054). The shader
        // multiplies this by light as a linear reflectance, so handing it the
        // encoded value made anthracite grey five and a half times too bright
        // while leaving white almost correct -- a non-uniform error no exposure
        // control can undo, and the reason the image read as flat and washed
        // out. Measured with SurfaceTemplateDump on 2026-08-21; see
        // ColourSpace.hpp for the table.
        const ModelerAPI::Color c = material.GetSurfaceColor ();
        m.r = SrgbToLinear (static_cast<float> (c.red));
        m.g = SrgbToLinear (static_cast<float> (c.green));
        m.b = SrgbToLinear (static_cast<float> (c.blue));

        // ⚠️ THE FLIP. ModelerAPI's transparency is 1 = INVISIBLE; alpha is
        // 1 = OPAQUE. Forgetting it renders the whole building as glass and the
        // glass as concrete.
        const double transparency = material.GetTransparency ();
        m.alpha = static_cast<float> (1.0 - std::clamp (transparency, 0.0, 1.0));

        // GetShining is a percentage. Clamping before this conversion flattened
        // every measured surface above 1% to the same finish. The divisor is
        // measured, not assumed — see SurfaceMaterial::shininess.
        m.shininess = static_cast<float> (std::clamp (material.GetShining () / 100.0, 0.0, 1.0));

        // ⚠️ NO DIVISOR HERE, AND THE ASYMMETRY IS REAL. This channel already
        // arrives 0..1 while its neighbour above arrives 0..100, from the same
        // material object — measured on the live pool, not inferred from the
        // name. Adding a /100 "for consistency" collapses every surface onto
        // F0 = 0.0008 and kills the highlight the channel exists to carry.
        m.specular = static_cast<float> (std::clamp (material.GetSpecularReflection (), 0.0, 1.0));

        // ⚠️ SAME 0..1 SCALE AS ITS NEIGHBOUR ABOVE, measured on the same pool:
        // SurfaceTemplateDump's join read API diffuse 62 against MODEL 0.62.
        m.diffuse = static_cast<float> (std::clamp (material.GetDiffuseReflection (), 0.0, 1.0));

        // ⚠️ READ FOR THE CLASSIFIER, NOT FOR THE SHADER. A coloured specular
        // highlight is the only thing in Archicad's surface data that says
        // "conductor" -- there is no metalness field and no IOR -- so this is
        // what lets SurfaceClassifier tell brass from gloss paint without ever
        // looking at a name. See SurfaceMaterial::specularR.
        const ModelerAPI::Color sc = material.GetSpecularColor ();
        m.specularR = static_cast<float> (std::clamp (sc.red, 0.0, 1.0));
        m.specularG = static_cast<float> (std::clamp (sc.green, 0.0, 1.0));
        m.specularB = static_cast<float> (std::clamp (sc.blue, 0.0, 1.0));
        const ModelerAPI::Color emission = material.GetEmissionColor ();
        m.emission[0] = SrgbToLinear (float (emission.red));
        m.emission[1] = SrgbToLinear (float (emission.green));
        m.emission[2] = SrgbToLinear (float (emission.blue));
        m.name = material.GetName ().ToCStr ().Get ();
        ReadMaterialTexture (model, material, m, *table);

        table->Set (m);
    }
    return table;
}

// Archicad's own sun, for this project's place and moment. MAIN THREAD.
//
// ⚠️ ARCHICAD COMPUTES IT — do not write a solar-position model (plan §3). The
// convention (sunAngXY is CCW from +X in MODEL space; sunAngZ is altitude above
// the horizon) was settled against an independent NOAA calculation; the one
// place it is spelled out is NativeCommands/ProjectCommands.cpp →
// GetPlaceInfoCommand, and this is the second consumer of the same three lines
// of trigonometry rather than a second convention.
bool ReadEnvironment (EnvironmentUpload& out)
{
    API_PlaceInfo place = {};
    if (ACAPI_GeoLocation_GetPlaceSets (&place) != NoError)
        return false;

    // ⚠️ WHICH SUN ARCHICAD SHADES WITH IS ArchViz/ProjectSun.cpp'S QUESTION NOW,
    // AND ITS HEADER CARRIES THE REASONING THAT USED TO BE HERE - a project has
    // TWO independent suns, the 3D window uses the one in 3D Projection
    // Settings, and there are two opposite ways of getting it wrong (a frozen
    // cached angle, and a recompute that discards a typed sun). It moved because
    // a SECOND caller appeared: the graph's camera node captures the sun a
    // viewpoint was taken under, and a copy of this logic is precisely how the
    // viewer and the capture would come to disagree about the light in one
    // frame. The resolver logs its own provenance and the RE67 drift.
    const ProjectSun viewSun = ResolveProjectSun ();
    const double sunAngXY = viewSun.valid ? viewSun.sunAngXY : place.sunAngXY;
    const double sunAngZ = viewSun.valid ? viewSun.sunAngZ : place.sunAngZ;
    const std::string sunSource = viewSun.source;
    constexpr double kRadToDeg = 57.29577951308232;

    const double horizontal = std::cos (sunAngZ);
    out.sunX = static_cast<float> (horizontal * std::cos (sunAngXY));
    out.sunY = static_cast<float> (horizontal * std::sin (sunAngXY));
    out.sunZ = static_cast<float> (std::sin (sunAngZ));
    out.ambient = 0.35f;
    out.sunBelowHorizon = (sunAngZ <= 0.0);

    out.northDegrees = static_cast<float> (place.north * kRadToDeg);

    // Where the sun came from, so the HUD can say it without the user opening a
    // log. The place and moment travel with it because "the viewer's sun is
    // wrong" and "the viewer is looking at a different project's place settings"
    // are one symptom until these numbers can be read side by side with
    // Archicad's own dialog.
    out.latitudeDegrees = static_cast<float> (place.latitude);
    out.longitudeDegrees = static_cast<float> (place.longitude);
    out.altitudeMetres = static_cast<float> (place.altitude);
    out.year = place.year;
    out.month = place.month;
    out.day = place.day;
    out.hour = place.hour;
    out.minute = place.minute;
    out.summerTime = place.sumTime;
    out.timeZoneMinutes = place.timeZoneInMinutes;

    // The calendar's own answer, for comparison ONLY. A project whose stored sun
    // was typed by hand will differ here, and that difference is information --
    // it is the difference this function used to silently prefer.
    // ⚠️ COLLECTED, NOT WRITTEN. Every line this function can
    // produce goes into one block that is emitted together or not at all, so the
    // change test below sees the whole reading rather than a line of it.
    std::vector<std::string> lines;

    API_PlaceInfo computed = place;
    if (ACAPI_GeoLocation_CalcSunOnPlace (&computed) == NoError) {
        out.computedAzimuthDegrees = static_cast<float> (computed.sunAngXY * kRadToDeg);
        out.computedAltitudeDegrees = static_cast<float> (computed.sunAngZ * kRadToDeg);
        out.haveComputedSun = true;
        const double azDelta = std::abs ((computed.sunAngXY - place.sunAngXY) * kRadToDeg);
        const double altDelta = std::abs ((computed.sunAngZ - place.sunAngZ) * kRadToDeg);
        if (azDelta > 0.5 || altDelta > 0.5)
            lines.push_back ("ArchViz sun ⚠ the STORED sun and the one this project's date/time imply "
                             "DISAGREE (azimuth by " +
                             std::to_string (azDelta) + " deg, altitude by " + std::to_string (altDelta) +
                             " deg). The stored one is used, because it is "
                             "what Archicad's own 3D window shades with -- this is the ordinary state "
                             "of a project whose sun was typed into the dialog rather than computed "
                             "from a date.");
    }

    // ⚠️ THIS CONVERSION NEEDS NO `north` TERM, AND ADDING ONE IS THE STANDING
    // TEMPTATION. It is character-for-character the one in
    // NativeCommands/ProjectCommands.cpp -> GetPlaceInfo, validated against an
    // independent NOAA calculation at TWO different project-north values -- the
    // second being what proved sunAngXY is a mathematical angle CCW from the
    // model's +X axis, already in model space, not a bearing from north. An
    // earlier round of this file carried a warning claiming the opposite; it was
    // a guess, and acting on it would have rotated every shadow by exactly the
    // amount it claimed to fix. Keep the two derivations identical.
    //
    // The COMPASS BEARING does need north (`north - sunAngXY`) and is what
    // Archicad's dialogs show. Both are logged, because confusing the two for
    // each other is the entire history of this function.
    double bearing = (place.north - sunAngXY) * kRadToDeg;
    bearing -= 360.0 * std::floor (bearing / 360.0);
    out.bearingDegrees = static_cast<float> (bearing);
    lines.push_back ("ArchViz sun IN USE, from " + sunSource + ": sunAngXY=" + std::to_string (sunAngXY * kRadToDeg) +
                     " deg (model space, CCW from +X)"
                     ", sunAngZ=" +
                     std::to_string (sunAngZ * kRadToDeg) + " deg, north=" + std::to_string (out.northDegrees) +
                     " deg -> compass bearing " + std::to_string (bearing) + " deg, model-space dir (" +
                     std::to_string (out.sunX) + ", " + std::to_string (out.sunY) + ", " + std::to_string (out.sunZ) +
                     ")");
    lines.push_back ("ArchViz sun NOT in use, for comparison -- the PLACE settings' stored pair: "
                     "sunAngXY=" +
                     std::to_string (place.sunAngXY * kRadToDeg) +
                     " deg, sunAngZ=" + std::to_string (place.sunAngZ * kRadToDeg) +
                     " deg. This is what the viewer used "
                     "before PLAT-RE67 and is NOT what the 3D window shades with unless it matches "
                     "the line above.");
    lines.push_back ("ArchViz place: lat " + std::to_string (place.latitude) + ", long " +
                     std::to_string (place.longitude) + ", altitude " + std::to_string (place.altitude) + " m, " +
                     std::to_string (place.year) + "-" + std::to_string (place.month) + "-" +
                     std::to_string (place.day) + " " + std::to_string (place.hour) + ":" +
                     std::to_string (place.minute) + (place.sumTime ? " (summer time)" : "") + ", tz " +
                     std::to_string (place.timeZoneInMinutes) + " min" +
                     (out.haveComputedSun
                          ? ("  |  the date/time would imply azimuth " + std::to_string (out.computedAzimuthDegrees) +
                             " deg, altitude " + std::to_string (out.computedAltitudeDegrees) + " deg")
                          : std::string ()));
    SayEnvironmentOnChange (lines);
    return true;
}

} // namespace archviz
} // namespace geomsrv
