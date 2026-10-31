#include "WorkflowState.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include <algorithm>
#include <limits>
#include <type_traits>
#include <cmath>
#include <iterator>

namespace tapioca::playground {
namespace {
template <class T> struct IsVector : std::false_type {};
template <class T, class A> struct IsVector<std::vector<T, A>> : std::true_type {};
template <class T> Value Encode (const T& value)
{
    if constexpr (std::is_same_v<T, bool>)
        return Value::Bool (value);
    else if constexpr (std::is_enum_v<T>)
        return Value::Integer (int64_t (value));
    else if constexpr (std::is_integral_v<T>)
        return Value::Integer (int64_t (value));
    else if constexpr (std::is_floating_point_v<T>)
        return Value::Double (double (value));
    else if constexpr (std::is_same_v<T, std::string>)
        return Value::String (value);
    else {
        json::JsonArray array;
        for (const auto& item : value)
            array.push_back (Encode (item));
        return Value::Array (std::move (array));
    }
}
template <class T> bool Decode (const Value& value, T& out)
{
    if constexpr (std::is_same_v<T, bool>)
        return value.AsBool (out);
    else if constexpr (std::is_same_v<T, std::string>)
        return value.AsString (out);
    else if constexpr (std::is_enum_v<T>) {
        int integer = 0;
        if (!Decode (value, integer))
            return false;
        out = T (integer);
        return true;
    }
    else if constexpr (std::is_arithmetic_v<T>) {
        double number = 0;
        if (!value.AsDouble (number) || !std::isfinite (number) ||
            number < double (std::numeric_limits<T>::lowest ()) || number > double (std::numeric_limits<T>::max ()))
            return false;
        if constexpr (std::is_integral_v<T>)
            if (std::trunc (number) != number)
                return false;
        out = T (number);
        return true;
    }
    else if constexpr (IsVector<T>::value) {
        const auto* array = value.AsArray ();
        if (!array || array->size () > 1000)
            return false;
        out.clear ();
        for (const auto& item : *array) {
            typename T::value_type decoded;
            if (!Decode (item, decoded))
                return false;
            out.push_back (std::move (decoded));
        }
        return true;
    }
    else {
        const auto* array = value.AsArray ();
        if (!array || array->size () != std::size (out))
            return false;
        for (size_t i = 0; i < array->size (); ++i)
            if (!Decode ((*array)[i], out[i]))
                return false;
        return true;
    }
}
template <class Visit> void Fields (MockServices& m, Visit visit)
{
#define HUD(name) visit ("hud." #name, m.hud.name)
    HUD (sunFilterLo);
    HUD (sunFilterHi);
    HUD (sunLowBlue);
    HUD (sunBlueThreshold);
    HUD (sunGradientStepped);
    HUD (sunBandCount);
    HUD (sunBandEdges);
    HUD (sunShadowOverlay);
    HUD (sunShotPreset);
    HUD (sunTimeFrom);
    HUD (sunTimeTo);
    HUD (sunInspect);
    HUD (sunView);
    HUD (sunFanInterval);
    HUD (sunRunYear);
    HUD (sunRunMonth);
    HUD (sunRunDay);
    HUD (sunRunTimestep);
    HUD (sunRunGrid);
    HUD (sunRunPreset);
    HUD (sunRunBackend);
    HUD (sunRunContext);
    HUD (sunRunIgnored);
    HUD (sunRunBounded);
    HUD (sunRunShowBounds);
    HUD (sunRunCentre);
    HUD (sunRunRadius);
    HUD (sunRunBoundsSeeded);
    HUD (sunSeenCommandedMode);
    HUD (visibilitySelection);
    HUD (visibilityFrom);
    HUD (visibilityTo);
    HUD (visibilityOrigin);
    HUD (visibilityGrid);
    HUD (visibilityAimPoints);
    HUD (visibilityConeDegrees);
    HUD (visibilityDirection);
    HUD (visibilityView);
    HUD (visibilityTarget);
    HUD (visibilityTargetSet);
    HUD (visibilityTargetEdited);
    HUD (visibilitySkyDegrees);
    HUD (visibilityGroundDegrees);
    HUD (visibilityLiveUpdate);
    HUD (visibilityLowRgba);
    HUD (visibilityHighRgba);
    HUD (viewpointFillRgba);
    HUD (viewpointEdgeRgba);
    HUD (viewpointBlockedRgba);
    HUD (viewpointFaceTarget);
    HUD (viewpointAlongPath);
    HUD (analysisPathAt);
    HUD (analysisProfileDirty);
    HUD (analysisProfileNote);
#undef HUD
#define POINT(name) visit ("point." #name, m.viewpoint.name)
    POINT (enabled);
    POINT (pointValid);
    POINT (contextAll);
    POINT (context);
    POINT (options.point);
    POINT (options.radius);
    POINT (options.mode);
    POINT (options.bearingDegrees);
    POINT (options.fieldOfViewDegrees);
    POINT (options.minElevationDegrees);
    POINT (options.maxElevationDegrees);
    POINT (options.surfaceCellMetres);
#undef POINT
#define EDIT(name) visit ("editor." #name, m.editor.name)
    EDIT (tool);
    EDIT (gizmo);
    EDIT (localAxes);
    EDIT (lasso);
    EDIT (mirrorKeeps);
    EDIT (snapEnabled);
    EDIT (snapModes);
    EDIT (snapGrid);
    EDIT (snapProximity);
    EDIT (angleStep);
    EDIT (edgeSnap);
    EDIT (polygonSides);
    EDIT (contours);
    EDIT (contourStep);
    EDIT (pathWidth);
    EDIT (actionDefaults.offsetDistance);
#undef EDIT
    visit ("sunDrawing", m.scene.sunStudy.drawing);
    visit ("analysisKind", m.scene.sunStudy.analysisKind);
    visit ("analysisTab", m.hud.analysisFocus.tab);
    visit ("shownOrigin", m.hud.analysisFocus.shownOrigin);
    visit ("studyId", m.scene.sunStudy.studyId);
    visit ("visibility.complete", m.visibility.complete);
    visit ("visibility.studyId", m.visibility.studyId);
    visit ("visibility.origin", m.visibility.origin);
    visit ("visibility.meanVisibility", m.visibility.meanVisibility);
    visit ("visibility.sampleCount", m.visibility.sampleCount);
    visit ("sun.stage", m.sun.stage);
    visit ("sun.runs", m.sun.runs);
    visit ("sun.studyId", m.sun.studyId);
}
} // namespace
Value WorkflowState (MockServices& services)
{
    json::JsonObject fields;
    Fields (services, [&] (const char* key, const auto& value) { fields[key] = Encode (value); });
    fields["editorDocument"] =
        Value::String (tapioca::geometry::WriteLandscapeDebugSnapshot (services.editor.edits.Doc ()));
    fields["editorLandscape"] = Value::Bool (services.editor.edits.Doc ().landscape.has_value ());
    json::JsonArray selection;
    for (const auto& ref : services.editor.selection.Items ())
        selection.push_back (Value::Object ({ { "entity", Value::Integer (int64_t (ref.entity)) },
                                              { "part", Value::Integer (int (ref.part)) },
                                              { "item", Value::Integer (int64_t (ref.item)) } }));
    fields["editorSelection"] = Value::Array (std::move (selection));
    if (services.hud.analysisPath) {
        const auto& path = *services.hud.analysisPath;
        fields["path"] = Value::Object ({ { "guid", Encode (path.guid) },
                                          { "points", Encode (path.points) },
                                          { "along", Encode (path.along) },
                                          { "closed", Encode (path.closed) } });
        std::vector<std::array<double, 4>> anchors;
        for (const auto& a : services.hud.analysisProfile.anchors)
            anchors.push_back ({ a.station, a.z, a.handleStation, a.handleZ });
        fields["profile"] = Encode (anchors);
    }
    return Value::Object (std::move (fields));
}
bool RestoreWorkflowState (MockServices& m, const Value& state)
{
    bool valid = true;
    Fields (m, [&] (const char* key, auto& value) {
        if (const auto* member = state.Find (key))
            valid &= Decode (*member, value);
    });
    if (!valid || m.hud.sunRunMonth < 1 || m.hud.sunRunMonth > 12 || m.hud.sunRunDay < 1 || m.hud.sunRunDay > 31 ||
        m.hud.sunRunYear < 1900 || m.hud.sunRunYear > 2200 || int (m.editor.tool) < 0 || int (m.editor.tool) > 7 ||
        m.hud.visibilityView < 0 || m.hud.visibilityView > 1 || int (m.viewpoint.options.mode) < 0 ||
        int (m.viewpoint.options.mode) > 2 || m.hud.analysisFocus.tab < 0 || m.hud.analysisFocus.tab > 3 ||
        m.hud.sunRunTimestep < 0 || m.hud.sunRunTimestep > 4 || m.hud.sunRunPreset < 0 || m.hud.sunRunPreset > 2 ||
        m.hud.sunRunBackend < 0 || m.hud.sunRunBackend > 1 || m.hud.sunFanInterval < 0 || m.hud.sunFanInterval > 5)
        return false;
    if (const auto* path = state.Find ("path")) {
        auto restored = std::make_shared<av::analysispath::Path> ();
        if (!Decode (Member (*path, "guid"), restored->guid) || !Decode (Member (*path, "points"), restored->points) ||
            !Decode (Member (*path, "along"), restored->along) ||
            !Decode (Member (*path, "closed"), restored->closed) || restored->points.size () < 2 ||
            restored->along.size () != restored->points.size () || restored->along.front () != 0 ||
            !std::is_sorted (restored->along.begin (), restored->along.end ()) || restored->Length () <= 0)
            return false;
        std::vector<std::array<double, 4>> anchors;
        if (!Decode (Member (state, "profile"), anchors) || anchors.size () < 2)
            return false;
        for (const auto& a : anchors)
            m.hud.analysisProfile.anchors.push_back ({ a[0], a[1], a[2], a[3] });
        av::analysisprofile::Fit (m.hud.analysisProfile, restored->Length ());
        m.hud.analysisPath = std::move (restored);
    }
    const auto text = Text (state, "editorDocument");
    if (!text.empty ()) {
        geomsrv::edit::Document doc;
        std::string error;
        if (!tapioca::geometry::ReadLandscape (text, doc, error))
            return false;
        if (!Flag (state, "editorLandscape"))
            doc.landscape.reset ();
        m.editor.edits = geomsrv::edit::Session (std::move (doc));
    }
    if (const auto* selection = Member (state, "editorSelection").AsArray ()) {
        std::vector<geomsrv::edit::Ref> refs;
        for (const auto& item : *selection) {
            geomsrv::edit::Ref ref;
            const double entity = Number (item, "entity"), part = Number (item, "part"), id = Number (item, "item");
            if (entity < 0 || entity > 1000000 || id < 0 || id > 1000000 || part < 0 || part > 7)
                return false;
            ref.entity = geomsrv::edit::Id (entity);
            ref.part = geomsrv::edit::Part (int (part));
            ref.item = geomsrv::edit::Id (id);
            if (!geomsrv::edit::Exists (m.editor.edits.Doc (), ref))
                return false;
            refs.push_back (ref);
        }
        m.editor.selection.Set (refs);
    }
    m.hud.analysisFocus.selectTab = m.hud.analysisFocus.tab;
    return true;
}
} // namespace tapioca::playground
