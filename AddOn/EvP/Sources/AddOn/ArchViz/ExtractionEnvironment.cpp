// ArchViz/ExtractionEnvironment — the surface pool and the sun. See the header
// for why these two live apart from the slicing machinery in
// ExtractionThread.cpp.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/ExtractionEnvironment.hpp"

#include "ArchViz/ArchVizLog.hpp" // ArchVizLog
#include "ArchViz/MaterialTable.hpp"
#include "ArchViz/SceneCmdQueue.hpp" // EnvironmentUpload

#include <AttributeIndex.hpp>
#include <Model.hpp>
#include <ModelMaterial.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {

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

        const ModelerAPI::Color c = material.GetSurfaceColor ();
        m.r = static_cast<float> (c.red);
        m.g = static_cast<float> (c.green);
        m.b = static_cast<float> (c.blue);

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
        m.name = material.GetName ().ToCStr ().Get ();

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

    // ⚠️ THE STORED ANGLES ARE AUTHORITATIVE, AND THIS USED TO CALL
    // ACAPI_GeoLocation_CalcSunOnPlace INSTEAD. That was wrong, and the way it
    // was wrong is invisible in most projects.
    //
    // Archicad's Sun dialog offers TWO ways to set the sun: pick a date, time and
    // place and let it compute the angles, or TYPE THE AZIMUTH AND ALTITUDE
    // DIRECTLY. In the second case the stored `sunAngXY`/`sunAngZ` are the user's
    // numbers and the stored date/time is whatever it happened to be --
    // CalcSunOnPlace then overwrites the user's sun with the one the calendar
    // implies, and the viewer lights the model from somewhere Archicad's own 3D
    // window plainly does not. That is exactly the live report: Archicad
    // 240 deg / 35 deg, the viewer -80 deg / 45 deg. The azimuths differ by a
    // convention; THE ALTITUDES CANNOT, and 45 against 35 is what says the two
    // are not reading the same sun at all.
    //
    // The old comment justified the recompute as "GetPlaceSets returns what was
    // last written, with no guarantee it followed the last date edit". If that is
    // ever true it is Archicad's own 3D window's problem too -- and matching that
    // window is the entire requirement (SceneCmdQueue.hpp, EnvironmentUpload).
    // The recomputed pair is still LOGGED below, so a project where the two
    // disagree names itself instead of being silently resolved one way.
    constexpr double kRadToDeg = 57.29577951308232;
    constexpr double kDegToRad = 0.017453292519943295;

    // ⚠️ AND THE PLACE SUN IS STILL NOT THE ONE ARCHICAD SHADES WITH (PLAT-RE67,
    // confirmed 2026-08-14). Everything above is about which of the PLACE's two
    // answers to believe, and the answer turned out to be neither: a project
    // carries a SECOND, independent sun in 3D Projection Settings -> Sun
    // Position (an API_SunAngleSettings inside API_PerspPars/API_AxonoPars), the
    // 3D window shades with THAT one, and the two dialogs drift apart in
    // silence. The project that found it had the place at 2018-08-20 12:00
    // (altitude 47.5 deg) and the view at 2017-03-22 10:00 (28.4 deg) -- a
    // 19-degree error that looked for weeks like a convention bug.
    //
    // The place sun stays as the FALLBACK: if the projection settings cannot be
    // read there is still a sun, and it is the one this file used to use.
    double sunAngXY = place.sunAngXY;
    double sunAngZ = place.sunAngZ;
    std::string sunSource = "PLACE settings (fallback -- the 3D projection settings did not read)";

    API_3DProjectionInfo projection = {};
    if (ACAPI_View_Get3DProjectionSets (&projection) == NoError) {
        // The union is discriminated by isPersp and both arms carry the same
        // settings struct; reading the wrong arm yields plausible garbage rather
        // than an error.
        const API_SunAngleSettings& viewSun =
            projection.isPersp ? projection.u.persp.sunAngSets : projection.u.axono.sunAngSets;
        if (viewSun.sunPosOpt == API_SunPosition_GivenByAngles) {
            // ⚠️ DEGREES, AND THE SAME ANGLE CONVENTION AS sunAngXY -- MEASURED,
            // NOT DOCUMENTED. The DevKit says only "rotation angle of the Sun
            // around the target": no unit, no zero. The 2026-08-14 Custom-mode
            // run had sunAzimuth = 312.524045 while CalcSunOnPlace gave
            // sunAngXY = 312.52 deg for the same view, so the field is degrees
            // and is NOT the compass bearing (137.5 deg in that project).
            //
            // ⚠️ WHAT THAT RUN DOES *NOT* SETTLE, and the next person deserves
            // to know: project north was 90 deg, so a convention differing from
            // sunAngXY by a north term cannot be ruled out -- the exact trap
            // that made `compass = 90 - sunAngXY` look correct for a whole
            // round (GetPlaceInfoCommand). Re-run DiligentShadowProbe at a
            // different project north before treating this as settled.
            sunAngXY = viewSun.sunAzimuth * kDegToRad;
            sunAngZ = viewSun.sunAltitude * kDegToRad;
            sunSource = "3D projection settings, TYPED angles (Custom)";
        }
        else {
            // Date-and-Time mode. The dialog sets a moment but never a place, so
            // the view's date is evaluated against the PROJECT's latitude,
            // longitude and time zone -- and the elevation exists nowhere in the
            // UI at all, which is why it has to be computed rather than read.
            API_PlaceInfo viewMoment = place;
            viewMoment.year = viewSun.year;
            viewMoment.month = viewSun.month;
            viewMoment.day = viewSun.day;
            viewMoment.hour = viewSun.hour;
            viewMoment.minute = viewSun.minute;
            viewMoment.second = viewSun.second;
            viewMoment.sumTime = viewSun.summerTime;
            if (ACAPI_GeoLocation_CalcSunOnPlace (&viewMoment) == NoError) {
                sunAngXY = viewMoment.sunAngXY;
                sunAngZ = viewMoment.sunAngZ;
                sunSource = "3D projection settings, DATE " + std::to_string (viewSun.year) + "-" +
                            std::to_string (viewSun.month) + "-" + std::to_string (viewSun.day) + " " +
                            std::to_string (viewSun.hour) + ":" + std::to_string (viewSun.minute);
            }
            else {
                ArchVizLog ("ArchViz sun ⚠ the 3D view is in Date-and-Time mode but CalcSunOnPlace "
                            "refused its date; falling back to the PLACE sun, which is very "
                            "probably not what the 3D window is shading with.");
            }
        }

        // Always logged, never silently resolved: the gap between the sun in use
        // and the place sun IS the RE67 drift, and a project where they agree
        // cannot be used to test any of this.
        const double azDelta = std::abs ((sunAngXY - place.sunAngXY) * kRadToDeg);
        const double altDelta = std::abs ((sunAngZ - place.sunAngZ) * kRadToDeg);
        if (azDelta > 0.5 || altDelta > 0.5)
            ArchVizLog ("ArchViz sun: the 3D VIEW's sun and the PROJECT PLACE's differ (azimuth by " +
                        std::to_string (azDelta) + " deg, altitude by " + std::to_string (altDelta) +
                        " deg). The VIEW's is used -- it is what Archicad's own 3D window shades "
                        "with (PLAT-RE67). This is the ordinary state of a project whose 3D "
                        "Projection Settings carry their own Sun Position, and is not an error.");
    }
    else {
        ArchVizLog ("ArchViz sun ⚠ ACAPI_View_Get3DProjectionSets failed, so the 3D window's OWN "
                    "sun could not be read and the project place's is being used instead. "
                    "Shadows will not match Archicad's unless the two happen to agree.");
    }

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
    API_PlaceInfo computed = place;
    if (ACAPI_GeoLocation_CalcSunOnPlace (&computed) == NoError) {
        out.computedAzimuthDegrees = static_cast<float> (computed.sunAngXY * kRadToDeg);
        out.computedAltitudeDegrees = static_cast<float> (computed.sunAngZ * kRadToDeg);
        out.haveComputedSun = true;
        const double azDelta = std::abs ((computed.sunAngXY - place.sunAngXY) * kRadToDeg);
        const double altDelta = std::abs ((computed.sunAngZ - place.sunAngZ) * kRadToDeg);
        if (azDelta > 0.5 || altDelta > 0.5)
            ArchVizLog ("ArchViz sun ⚠ the STORED sun and the one this project's date/time imply "
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
    ArchVizLog ("ArchViz sun IN USE, from " + sunSource + ": sunAngXY=" + std::to_string (sunAngXY * kRadToDeg) +
                " deg (model space, CCW from +X)"
                ", sunAngZ=" +
                std::to_string (sunAngZ * kRadToDeg) + " deg, north=" + std::to_string (out.northDegrees) +
                " deg -> compass bearing " + std::to_string (bearing) + " deg, model-space dir (" +
                std::to_string (out.sunX) + ", " + std::to_string (out.sunY) + ", " + std::to_string (out.sunZ) + ")");
    ArchVizLog ("ArchViz sun NOT in use, for comparison -- the PLACE settings' stored pair: "
                "sunAngXY=" +
                std::to_string (place.sunAngXY * kRadToDeg) +
                " deg, sunAngZ=" + std::to_string (place.sunAngZ * kRadToDeg) +
                " deg. This is what the viewer used "
                "before PLAT-RE67 and is NOT what the 3D window shades with unless it matches "
                "the line above.");
    ArchVizLog ("ArchViz place: lat " + std::to_string (place.latitude) + ", long " + std::to_string (place.longitude) +
                ", altitude " + std::to_string (place.altitude) + " m, " + std::to_string (place.year) + "-" +
                std::to_string (place.month) + "-" + std::to_string (place.day) + " " + std::to_string (place.hour) +
                ":" + std::to_string (place.minute) + (place.sumTime ? " (summer time)" : "") + ", tz " +
                std::to_string (place.timeZoneInMinutes) + " min" +
                (out.haveComputedSun
                     ? ("  |  the date/time would imply azimuth " + std::to_string (out.computedAzimuthDegrees) +
                        " deg, altitude " + std::to_string (out.computedAltitudeDegrees) + " deg")
                     : std::string ()));
    return true;
}

} // namespace archviz
} // namespace geomsrv
