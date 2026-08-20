#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ProjectCommands.hpp"
#include "AddOnCommands.hpp"                    // ProjectInfoField is declared there
#include "NativeCommands/CommandRegistration.hpp"

#include <cmath>

namespace geomsrv {

namespace {

// ---------------------------------------------------------------------------
// EvP.GetStories {} -> the project's stories, bottom to top.
//
// Native rather than proxied through Tapir: the palette needs this to populate an
// `evp.Story` picker, and a UI control must not depend on an add-on we do not
// ship. `level` is world Z, the same frame as geometry, which is what makes a
// story index meaningful next to a mesh coordinate.
// ---------------------------------------------------------------------------
class GetStoriesCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetStories"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        API_StoryInfo info = {};
        const GSErrCode err = ACAPI_ProjectSetting_GetStorySettings (&info);
        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_ProjectSetting_GetStorySettings", err,
                                                                  "reading the story list"));
        }

        GS::Array<GS::Int32>     indices;
        GS::Array<GS::UniString> names;
        GS::Array<double>        levels;

        if (info.data != nullptr) {
            const short count = info.lastStory - info.firstStory + 1;
            for (short i = 0; i < count; ++i) {
                const API_StoryType& story = (*info.data)[i];
                indices.Push ((GS::Int32) story.index);
                names.Push (GS::UniString (story.uName));
                levels.Push (story.level);
            }
            // The handle is ours to free — the SDK allocated it for this call.
            BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
        }

        os.Add ("firstStory", (GS::Int32) info.firstStory);
        os.Add ("lastStory", (GS::Int32) info.lastStory);
        os.Add ("actStory", (GS::Int32) info.actStory);
        os.Add ("indices", indices);
        os.Add ("names", names);
        os.Add ("levels", levels);      // world Z, same frame as geometry
        os.Add ("count", (GS::Int32) indices.GetSize ());
        return os;
    }
};

// Read every project-info field as three parallel arrays: UI name / database key /
// value. In Archicad a custom project-info field IS an autotext, so this is the
// one source both GetProjectInfoCommand (all fields) and ProjectInfoField (one
// field by name) share — the field semantics live here, once.
static GSErrCode ReadProjectInfoFields (GS::Array<GS::UniString>& names,
                                        GS::Array<GS::UniString>& keys,
                                        GS::Array<GS::UniString>& values)
{
    GS::Array<GS::ArrayFB<GS::UniString, 3>> autotexts;
    const GSErrCode aerr = ACAPI_AutoText_GetAutoTexts (&autotexts, APIAutoText_All);
    if (aerr != NoError)
        return aerr;
    for (const GS::ArrayFB<GS::UniString, 3>& t : autotexts) {
        if (t.GetSize () < 3)
            continue;                            // stay parallel; skip a malformed triplet
        names.Push (t[0]);                       // description / UI name
        keys.Push (t[1]);                        // database key
        values.Push (t[2]);                      // value
    }
    return NoError;
}

// ---------------------------------------------------------------------------
// EvP.GetProjectInfo {} -> project name/path + all project-info autotext fields.
//
// Absorbs the last two Tapir calls in MassingFeasibility: Tapir.GetProjectInfo
// (project name/path) and Tapir.GetProjectInfoFields (the custom "Sklypo plotas"
// plot-area field). Both are plain reads — no undo scope, no writes.
//
//   projectName/projectPath : ACAPI_ProjectOperation_Project (API_ProjectInfo).
//   field* parallel arrays   : ACAPI_AutoText_GetAutoTexts. In Archicad a custom
//     project-info field IS an autotext; each triplet is (description/UI name,
//     database key, value) per the header. fieldNames[i] is what the user sees
//     ("Sklypo plotas"); fieldValues[i] is its string value.
// ---------------------------------------------------------------------------
class GetProjectInfoCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetProjectInfo"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        API_ProjectInfo info = {};               // destructor frees the UniString*s
        const GSErrCode perr = ACAPI_ProjectOperation_Project (&info);
        if (perr != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_ProjectOperation_Project", perr,
                                                                  "reading project name/path"));
        }

        os.Add ("projectName", info.projectName != nullptr ? *info.projectName : GS::UniString ());
        os.Add ("projectPath", info.projectPath != nullptr ? *info.projectPath : GS::UniString ());
        os.Add ("untitled", info.untitled);

        GS::Array<GS::UniString> fieldNames, fieldKeys, fieldValues;

        const GSErrCode aerr = ReadProjectInfoFields (fieldNames, fieldKeys, fieldValues);
        if (aerr != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_AutoText_GetAutoTexts", aerr,
                                                                  "reading the Project Info autotext fields"));
        }

        os.Add ("fieldNames", fieldNames);
        os.Add ("fieldKeys", fieldKeys);
        os.Add ("fieldValues", fieldValues);
        os.Add ("count", (GS::Int32) fieldNames.GetSize ());
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.GetPlaceInfo { year?, month?, day?, hour?, minute?, second? }
//   -> geo location, project north, and the SUN ANGLES for that moment.
//
// E25's other half. Nothing under Sources/AddOn/ touched API_PlaceInfo before
// this, so the project's longitude/latitude/altitude/north — and Archicad's own
// sun position — were simply unreachable. The daylight study (ModelViewer R5)
// needs all of them.
//
// ⚠️ ARCHICAD COMPUTES THE SUN POSITION. ACAPI_GeoLocation_CalcSunOnPlace fills
// sunAngXY/sunAngZ from the place and the timestamp already in the struct, so NO
// solar-position formula has to be written, and more importantly none has to be
// kept in agreement with the one Archicad renders shadows with. Do not
// reimplement this in Python; a study whose sun disagrees with the model's own
// shadows is worse than no study.
//
// ⚠️ THE UNITS ARE MIXED, and that is the DevKit's doing, not a choice here:
// longitude/latitude are DEGREES, altitude is METRES, and north/sunAngXY/sunAngZ
// are RADIANS. Both spellings are returned (`north` + `northDeg`, ...) because
// this is exactly the mix-up that produces a plausible-looking shadow pointing
// the wrong way.
//
// With no timestamp the project's own date/time is used — i.e. what the Sun
// dialog shows. Any subset of the fields may be given; the rest keep the
// project's values, so asking for one hour on the project's own date is
// { "hour": 15 }.
// ---------------------------------------------------------------------------
class GetPlaceInfoCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetPlaceInfo"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        API_PlaceInfo place = {};
        const GSErrCode err = ACAPI_GeoLocation_GetPlaceSets (&place);
        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_GeoLocation_GetPlaceSets", err,
                                                                  "reading the project's geo location and sun settings"));
        }

        // Which fields did the caller override? Reported back so a study can log
        // the moment it actually asked about rather than the one it meant to.
        bool overridden = false;
        overridden |= ApplyTime (params, "year",   place.year);
        overridden |= ApplyTime (params, "month",  place.month);
        overridden |= ApplyTime (params, "day",    place.day);
        overridden |= ApplyTime (params, "hour",   place.hour);
        overridden |= ApplyTime (params, "minute", place.minute);
        overridden |= ApplyTime (params, "second", place.second);

        // Recompute even when nothing was overridden: GetPlaceSets returns the
        // stored angles, and there is no guarantee they were recomputed after the
        // last edit to the date. One cheap call removes the whole question.
        const GSErrCode sunErr = ACAPI_GeoLocation_CalcSunOnPlace (&place);
        if (sunErr != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_GeoLocation_CalcSunOnPlace", sunErr,
                                                                  "computing the sun position for the requested moment"));
        }

        os.Add ("longitude", place.longitude);          // degrees
        os.Add ("latitude",  place.latitude);           // degrees
        os.Add ("altitude",  place.altitude);           // metres

        os.Add ("north",     place.north);              // radians, as the DevKit gives it
        os.Add ("northDeg",  Degrees (place.north));

        os.Add ("sunAngXY",    place.sunAngXY);         // radians
        os.Add ("sunAngZ",     place.sunAngZ);
        os.Add ("sunAngXYDeg", Degrees (place.sunAngXY));
        os.Add ("sunAngZDeg",  Degrees (place.sunAngZ));

        // THE CONVENTION, SETTLED 2026-08-03 against an independent NOAA solar
        // calculation — twice, at two different project-north values, which is
        // what it took:
        //
        //   * sunAngZ  is the altitude ABOVE THE HORIZON.
        //   * sunAngXY is a MATHEMATICAL angle, counterclockwise from the model's
        //     +X axis. It is already in MODEL/WORLD space, so the vector below is
        //     a plain spherical->cartesian and needs no north term.
        //   * north is the direction of GEOGRAPHIC NORTH as the same kind of
        //     angle (CCW from +X); its 90 deg default puts north along +Y.
        //
        // ⚠️ THE COMPASS BEARING NEEDS `north` AND THE FIRST RUN HID THAT. Round
        // one checked out perfectly with `compass = 90 - sunAngXY` — because that
        // project had north at exactly 90 deg, the one value where the north term
        // vanishes. At north = 10 deg that rule was off by exactly 80 deg on all
        // four samples, and `compass = north - sunAngXY` was exact on all four.
        // So sunAzimuthDeg is computed here, from north, rather than left to a
        // consumer to rediscover the same way.
        const double horizontal = std::cos (place.sunAngZ);
        os.Add ("sunDirX", horizontal * std::cos (place.sunAngXY));   // model space
        os.Add ("sunDirY", horizontal * std::sin (place.sunAngXY));
        os.Add ("sunDirZ", std::sin (place.sunAngZ));

        // Clockwise from geographic north — the bearing a daylight report prints
        // ("the sun is at 193 deg"), as opposed to the model-space angle above.
        double azimuth = Degrees (place.north - place.sunAngXY);
        azimuth = azimuth - 360.0 * std::floor (azimuth / 360.0);      // into [0, 360)
        os.Add ("sunAzimuthDeg", azimuth);
        os.Add ("sunAltitudeDeg", Degrees (place.sunAngZ));            // = sunAngZDeg, named for readers

        os.Add ("year",   (GS::Int32) place.year);
        os.Add ("month",  (GS::Int32) place.month);
        os.Add ("day",    (GS::Int32) place.day);
        os.Add ("hour",   (GS::Int32) place.hour);
        os.Add ("minute", (GS::Int32) place.minute);
        os.Add ("second", (GS::Int32) place.second);

        os.Add ("summerTime",        place.sumTime);
        os.Add ("timeZoneInMinutes", (GS::Int32) place.timeZoneInMinutes);
        os.Add ("timeOverridden",    overridden);
        return os;
    }

private:
    // One date/time field, if the caller gave it. Returns whether it did.
    // Negative and absurd values are IGNORED rather than clamped: a caller that
    // passes -1 for `hour` has a bug, and silently turning it into 0 or 23 hides
    // it behind a sun that is merely in the wrong place.
    static bool ApplyTime (const GS::ObjectState& params, const char* key, unsigned short& field)
    {
        GS::Int32 value = -1;
        if (!params.Get (key, value) || value < 0 || value > 65535)
            return false;
        field = (unsigned short) value;
        return true;
    }

    static double Degrees (double radians)
    {
        constexpr double pi = 3.14159265358979323846;
        return radians * 180.0 / pi;
    }
};

// ---------------------------------------------------------------------------
// EvP.GetViewSunInfo {} -> the sun the 3D WINDOW shades with.
//
// ⚠️ THIS IS A DIFFERENT SUN FROM GetPlaceInfo's, AND THAT IS THE WHOLE POINT
// (PLAT-RE67, 2026-08-14). A project carries TWO independent sun definitions:
//
//   * the PLACE sun -- Project Location's lat/long/date/time, what
//     ACAPI_GeoLocation_GetPlaceSets returns, what GetPlaceInfo reports;
//   * the VIEW sun -- 3D Projection Settings' own "Sun Position" panel, an
//     API_SunAngleSettings living inside the projection parameters.
//
// Archicad's 3D window shades with the VIEW one. They are edited in different
// dialogs and drift apart silently: the run that found this had the place at
// 2018-08-20 12:00 (altitude 47.5 deg) and the view at 2017-03-22 10:00
// (altitude ~28 deg), and the viewer was lighting the model from the first.
//
// ⚠️ `sunAzimuth`/`sunAltitude` ARE RETURNED RAW AND UNINTERPRETED. The DevKit
// documents sunAzimuth only as "rotation angle of the Sun around the target" --
// it does NOT say degrees or radians, and it does NOT say whether it shares
// sunAngXY's CCW-from-model-+X convention or is a compass bearing. Nothing here
// guesses; the caller prints both readings and a human settles it by looking.
// Until then the trustworthy number is `computed*`, which comes from Archicad's
// OWN solar math (CalcSunOnPlace) run on the VIEW's date, and which is also the
// only way to learn the elevation while the dialog is in Date-and-Time mode --
// that mode does not display an altitude at all.
// ---------------------------------------------------------------------------
class GetViewSunInfoCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetViewSunInfo"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        API_3DProjectionInfo projection = {};
        const GSErrCode projErr = ACAPI_View_Get3DProjectionSets (&projection);
        if (projErr != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_View_Get3DProjectionSets", projErr,
                                                                  "reading the 3D window's projection settings for its sun"));
        }

        // The union is discriminated by isPersp, and the two arms carry the same
        // API_SunAngleSettings -- reading the wrong arm yields plausible garbage
        // rather than an error, which is why this is a switch and not a guess.
        const API_SunAngleSettings& sun = projection.isPersp ? projection.u.persp.sunAngSets
                                                            : projection.u.axono.sunAngSets;

        API_PlaceInfo place = {};
        const GSErrCode placeErr = ACAPI_GeoLocation_GetPlaceSets (&place);
        if (placeErr != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_GeoLocation_GetPlaceSets", placeErr,
                                                                  "reading the project place the view's sun date is evaluated against"));
        }

        GS::ObjectState os;
        os.Add ("isPersp", projection.isPersp);
        os.Add ("sunPositionMode", GS::UniString (sun.sunPosOpt == API_SunPosition_GivenByAngles
                                                      ? "byAngles" : "byDate"));
        os.Add ("sunAzimuthRaw",  sun.sunAzimuth);
        os.Add ("sunAltitudeRaw", sun.sunAltitude);

        os.Add ("viewYear",   (GS::Int32) sun.year);
        os.Add ("viewMonth",  (GS::Int32) sun.month);
        os.Add ("viewDay",    (GS::Int32) sun.day);
        os.Add ("viewHour",   (GS::Int32) sun.hour);
        os.Add ("viewMinute", (GS::Int32) sun.minute);
        os.Add ("viewSecond", (GS::Int32) sun.second);
        os.Add ("viewSummerTime", sun.summerTime);

        os.Add ("north",    place.north);
        os.Add ("northDeg", Degrees (place.north));

        // The VIEW's moment, on the PROJECT's place: the sun dialog sets a date
        // but never a latitude, so the place still decides where on earth that
        // date is being observed from. Timezone likewise stays the project's.
        place.year    = sun.year;
        place.month   = sun.month;
        place.day     = sun.day;
        place.hour    = sun.hour;
        place.minute  = sun.minute;
        place.second  = sun.second;
        place.sumTime = sun.summerTime;

        const GSErrCode sunErr = ACAPI_GeoLocation_CalcSunOnPlace (&place);
        if (sunErr != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_GeoLocation_CalcSunOnPlace", sunErr,
                                                                  "computing the sun for the 3D view's own date and time"));
        }

        // Same three lines as GetPlaceInfo, and deliberately identical: sunAngXY
        // is CCW from the model's +X and already in model space, sunAngZ is the
        // altitude above the horizon. See the block in GetPlaceInfoCommand for
        // the NOAA validation that settled it -- do NOT add a north term here.
        os.Add ("computedSunAngXY",    place.sunAngXY);
        os.Add ("computedSunAngZ",     place.sunAngZ);
        os.Add ("computedSunAngXYDeg", Degrees (place.sunAngXY));
        os.Add ("computedSunAngZDeg",  Degrees (place.sunAngZ));

        const double horizontal = std::cos (place.sunAngZ);
        os.Add ("computedSunDirX", horizontal * std::cos (place.sunAngXY));
        os.Add ("computedSunDirY", horizontal * std::sin (place.sunAngXY));
        os.Add ("computedSunDirZ", std::sin (place.sunAngZ));

        double azimuth = Degrees (place.north - place.sunAngXY);
        azimuth = azimuth - 360.0 * std::floor (azimuth / 360.0);
        os.Add ("computedAzimuthDeg",  azimuth);                     // compass, CW from north
        os.Add ("computedAltitudeDeg", Degrees (place.sunAngZ));     // the elevation the dialog hides
        return os;
    }

private:
    static double Degrees (double radians)
    {
        constexpr double pi = 3.14159265358979323846;
        return radians * 180.0 / pi;
    }
};

const NativeCommandRegistration ProjectCommandRegistrations[] = {
    { "GetStories", &MakeRegisteredNativeCommand<GetStoriesCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({
            "type":"object",
            "properties":{
                "firstStory":{"type":"integer"},
                "lastStory":{"type":"integer"},
                "actStory":{"type":"integer"},
                "indices":{"type":"array","items":{"type":"integer"}},
                "names":{"type":"array","items":{"type":"string"}},
                "levels":{"type":"array","items":{"type":"number"}},
                "count":{"type":"integer"}
            },
            "additionalProperties":false,
            "required":["firstStory","lastStory","actStory","indices","names","levels","count"]
        })json" },
    { "GetProjectInfo", &MakeRegisteredNativeCommand<GetProjectInfoCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({
            "type":"object",
            "properties":{
                "projectName":{"type":"string"},
                "projectPath":{"type":"string"},
                "untitled":{"type":"boolean"},
                "fieldNames":{"type":"array","items":{"type":"string"}},
                "fieldKeys":{"type":"array","items":{"type":"string"}},
                "fieldValues":{"type":"array","items":{"type":"string"}},
                "count":{"type":"integer"}
            },
            "additionalProperties":false,
            "required":["projectName","projectPath","untitled","fieldNames","fieldKeys","fieldValues","count"]
        })json" },
    { "GetPlaceInfo", &MakeRegisteredNativeCommand<GetPlaceInfoCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "year":{"type":"integer"},
                "month":{"type":"integer"},
                "day":{"type":"integer"},
                "hour":{"type":"integer"},
                "minute":{"type":"integer"},
                "second":{"type":"integer"}
            },
            "additionalProperties":false
        })json",
      R"json({
            "type":"object",
            "properties":{
                "longitude":{"type":"number"},
                "latitude":{"type":"number"},
                "altitude":{"type":"number"},
                "north":{"type":"number"},
                "northDeg":{"type":"number"},
                "sunAngXY":{"type":"number"},
                "sunAngZ":{"type":"number"},
                "sunAngXYDeg":{"type":"number"},
                "sunAngZDeg":{"type":"number"},
                "sunDirX":{"type":"number"},
                "sunDirY":{"type":"number"},
                "sunDirZ":{"type":"number"},
                "sunAzimuthDeg":{"type":"number"},
                "sunAltitudeDeg":{"type":"number"},
                "year":{"type":"integer"},
                "month":{"type":"integer"},
                "day":{"type":"integer"},
                "hour":{"type":"integer"},
                "minute":{"type":"integer"},
                "second":{"type":"integer"},
                "summerTime":{"type":"boolean"},
                "timeZoneInMinutes":{"type":"integer"},
                "timeOverridden":{"type":"boolean"}
            },
            "additionalProperties":false,
            "required":["longitude","latitude","altitude","north","northDeg","sunAngXY","sunAngZ","sunAngXYDeg","sunAngZDeg","sunDirX","sunDirY","sunDirZ","sunAzimuthDeg","sunAltitudeDeg","year","month","day","hour","minute","second","summerTime","timeZoneInMinutes","timeOverridden"]
        })json" },
    { "GetViewSunInfo", &MakeRegisteredNativeCommand<GetViewSunInfoCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({
            "type":"object",
            "properties":{
                "isPersp":{"type":"boolean"},
                "sunPositionMode":{"type":"string"},
                "sunAzimuthRaw":{"type":"number"},
                "sunAltitudeRaw":{"type":"number"},
                "viewYear":{"type":"integer"},
                "viewMonth":{"type":"integer"},
                "viewDay":{"type":"integer"},
                "viewHour":{"type":"integer"},
                "viewMinute":{"type":"integer"},
                "viewSecond":{"type":"integer"},
                "viewSummerTime":{"type":"boolean"},
                "north":{"type":"number"},
                "northDeg":{"type":"number"},
                "computedSunAngXY":{"type":"number"},
                "computedSunAngZ":{"type":"number"},
                "computedSunAngXYDeg":{"type":"number"},
                "computedSunAngZDeg":{"type":"number"},
                "computedSunDirX":{"type":"number"},
                "computedSunDirY":{"type":"number"},
                "computedSunDirZ":{"type":"number"},
                "computedAzimuthDeg":{"type":"number"},
                "computedAltitudeDeg":{"type":"number"}
            },
            "additionalProperties":false,
            "required":["isPersp","sunPositionMode","sunAzimuthRaw","sunAltitudeRaw","viewYear","viewMonth","viewDay","viewHour","viewMinute","viewSecond","viewSummerTime","north","northDeg","computedSunAngXY","computedSunAngZ","computedSunAngXYDeg","computedSunAngZDeg","computedSunDirX","computedSunDirY","computedSunDirZ","computedAzimuthDeg","computedAltitudeDeg"]
        })json" },
};

}   // namespace

NativeCommandRegistrations GetProjectCommandRegistrations ()
{
    return MakeRegistrationView (ProjectCommandRegistrations);
}

// Declared in AddOnCommands.hpp. Lives here rather than in the registry because
// its only helper, ReadProjectInfoFields, is this file's — and is the same
// field-scan GetProjectInfoCommand exposes. Keeping both together means the
// helper never has to be exported for a single caller.
bool ProjectInfoField (const GS::UniString& needleText, GS::UniString& value)
{
    if (needleText.IsEmpty ())
        return false;

    GS::Array<GS::UniString> names, keys, values;
    if (ReadProjectInfoFields (names, keys, values) != NoError)
        return false;

    // ⚠️ KEY FIRST, EXACTLY; description second, by substring.
    //
    // The DevKit is explicit about which half is the identity: the triplet
    // ACAPI_AutoText_GetAutoTexts returns is (description, DATABASE KEY, value),
    // and ACAPI_AutoText_SetAnAutoText keys on `autotextDbKey`. The description is
    // what the user reads and edits; the key is what names the field. Two fields
    // can therefore share a description — or share a substring of one, which a
    // substring match makes likelier still ("Sklypo plotas" also matches "Sklypo
    // plotas bendras", and which one wins is array order, i.e. nothing).
    //
    // So an exact key match is tried first and settles the question. The
    // description path stays as the fallback because it is what a human writes in
    // a command's source (`default_from="project:Sklypo plotas"`) and what every
    // caller before the ProjectField picker already passes; dropping it would
    // break those with no gain.
    const GS::UniString needle = needleText.ToLowerCase ();
    for (UIndex i = 0; i < keys.GetSize (); ++i) {
        if (keys[i].ToLowerCase () == needle) {
            value = values[i];
            return true;
        }
    }
    for (UIndex i = 0; i < names.GetSize (); ++i) {
        if (names[i].ToLowerCase ().Contains (needle)) {
            value = values[i];
            return true;
        }
    }
    return false;
}

// Declared in AddOnCommands.hpp. The picker half of the pair above: the palette
// shows the DESCRIPTION and sends the KEY, so it needs both columns of the same
// scan. Sharing ReadProjectInfoFields is what guarantees a picked key always
// resolves in ProjectInfoField — two independent scans could disagree about which
// fields exist.
bool ProjectInfoFieldChoices (GS::Array<GS::UniString>& descriptions,
                             GS::Array<GS::UniString>& keys,
                             GS::Array<GS::UniString>& values)
{
    return ReadProjectInfoFields (descriptions, keys, values) == NoError;
}

} // namespace geomsrv
