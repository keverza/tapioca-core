#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ProjectEnv/ProjectSun.hpp"

#include <cmath>
#include <string>

// ⚠️ MOVED HERE VERBATIM FROM ExtractionEnvironment.cpp, COMMENTS AND ALL.
// Every warning below was paid for by a live run, and the reasoning is the
// artifact - not the six lines of branching it justifies. Two callers now share
// it: the viewer's environment upload, which lights the scene, and the graph's
// camera node, which captures the sun a viewpoint was taken under. A third copy
// is how the two would drift.

namespace geomsrv {

ProjectSun ResolveProjectSun ()
{
    ProjectSun resolved;

    API_PlaceInfo place = {};
    if (ACAPI_GeoLocation_GetPlaceSets (&place) != NoError)
        return resolved;

    constexpr double kRadToDeg = 57.29577951308232;
    constexpr double kDegToRad = 0.017453292519943295;
    (void) kRadToDeg;

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
                // The mode says a date and Archicad would not evaluate it. Reported
                // through `source` rather than logged: this is services-tier and
                // ArchVizLog is not, and a caller that cares can say it far better
                // than a line in a file nobody opens.
                sunSource = "PLACE settings (the 3D view is in Date-and-Time mode but "
                            "CalcSunOnPlace refused its date, so this is very probably NOT "
                            "what the 3D window is shading with)";
            }
        }

        // The DRIFT between this and the place sun is not measured here: a
        // caller holding both can measure it, and the one that cares already
        // logs it (ArchViz/ExtractionEnvironment.cpp). See ProjectSun::fromView.
    }

    resolved.valid = true;
    resolved.sunAngXY = sunAngXY;
    resolved.sunAngZ = sunAngZ;
    resolved.north = place.north;
    resolved.source = sunSource;
    // The place sun is the fallback and says so in its own source string; any
    // other source means the 3D projection's own settings were read.
    resolved.fromView = sunSource.rfind ("PLACE settings", 0) != 0;
    return resolved;
}

} // namespace geomsrv
