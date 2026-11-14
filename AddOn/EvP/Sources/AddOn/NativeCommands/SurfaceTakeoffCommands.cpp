// OVERLAY-INVARIANTS.md: seeded facade calculation, existing layer publication.
#include "NativeCommands/SurfaceTakeoffCommands.hpp"
#include "Geometry/SurfaceTakeoffController.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "NativeCommands/SurfaceCriteriaMetadata.hpp"
#include "AddOnCommands.hpp"
#include "ArchViz/SurfaceTakeoffOverlay.hpp"
#include "Geometry/ExteriorSurfaceRegion.hpp"
#include "Geometry/MeshStore.hpp"
#include "Geometry/SurfaceTakeoffResult.hpp"
#include "Python/MainThreadGate.hpp"
#include "Python/RunCancel.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace geomsrv {
namespace {
namespace ex = exposed;
namespace view = archviz::surfacetakeoff;
thread_local const std::function<bool ()>* t_externalCancel = nullptr;
thread_local bool t_controllerExecution = false;
GS::UniString Text (const std::string& value)
{
    return GS::UniString (value.c_str (), CC_UTF8);
}
std::string Utf8 (const GS::UniString& value)
{
    return value.ToCStr (0, MaxUSize, CC_UTF8).Get ();
}
std::string Guid (std::string value)
{
    if (value.size () == 38 && value.front () == '{' && value.back () == '}')
        value = value.substr (1, 36);
    std::transform (value.begin (), value.end (), value.begin (),
                    [] (unsigned char c) { return char (std::toupper (c)); });
    return value;
}
bool Current (const std::shared_ptr<const Snapshot>& snapshot)
{
    return snapshot && MeshStore::Get ().Current () == snapshot &&
           MeshStore::Get ().CaptureStamp () == snapshot->captureStamp;
}
bool RunEnded (uint64_t generation)
{
    auto& run = evp::RunCancel::Get ();
    return generation != 0 && (!run.IsRunning () || run.Generation () != generation || run.IsCancelled (generation));
}
bool FindMesh (const Snapshot& snapshot, const std::string& guid, size_t& index)
{
    size_t matches = 0;
    for (size_t i = 0; i < snapshot.meshes.size (); ++i)
        if (Guid (snapshot.meshes[i].guid) == Guid (guid)) {
            index = i;
            ++matches;
        }
    return matches == 1;
}
GS::ObjectState Element (const std::string& guid)
{
    GS::ObjectState id, item;
    id.Add ("guid", Text (guid));
    item.Add ("elementId", id);
    return item;
}
template <typename T> GS::Array<T> Array (const std::vector<T>& values)
{
    GS::Array<T> result;
    for (const auto& value : values)
        result.Push (value);
    return result;
}
const char kInput[] = R"json({
 "type":"object","additionalProperties":false,
 "properties":{
  "snapshotId":{"type":"integer","minimum":1},"captureStamp":{"type":"integer","minimum":1},
  "targetGuids":{"type":"array","minItems":1,"maxItems":64,"uniqueItems":true,"items":{"type":"string","minLength":1,"maxLength":38}},
  "coverGuids":{"type":"array","maxItems":128,"uniqueItems":true,"items":{"type":"string","minLength":1,"maxLength":38}},
  "coverScopeId":{"type":"string","minLength":1,"maxLength":256},
  "contactToleranceM":{"type":"number","minimum":0,"maximum":0.001},
  "regionMode":{"type":"string","enum":["seededFacade","allWallFaces","allElementFaces"]},
  "seeds":{"type":"array","maxItems":8,"items":{"type":"object","additionalProperties":false,
    "properties":{"guid":{"type":"string","minLength":1,"maxLength":38},"point":{"$ref":"#/$defs/vector"},"normal":{"$ref":"#/$defs/vector"}},"required":["guid","point","normal"]}},
   "surfaceAttributeIndices":{"type":"array","maxItems":512,"uniqueItems":true,"items":{"type":"integer","minimum":1}},
   "surfaceColors":{"type":"array","maxItems":512,"description":"Legacy compatibility input; highlights use the fixed selection color.","items":{"type":"integer","minimum":0,"maximum":4294967295}},
  "publishOverlay":{"type":"boolean"},"compareNative":{"type":"boolean"}
 },"required":["snapshotId","captureStamp","targetGuids","coverGuids","coverScopeId","seeds","surfaceAttributeIndices","publishOverlay","compareNative"],
 "$defs":{"vector":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}}}
})json";
const char kOutput[] = R"json({
 "type":"object","additionalProperties":false,"properties":{
  "snapshotId":{"type":"integer","minimum":1},"captureStamp":{"type":"integer","minimum":1},
  "classification":{"enum":["userExteriorSeeds-connectedCoplanar","allWallFaces-contactOnly","allElementFaces-contactOnly"]},"unit":{"const":"m2"},
  "coverScopeId":{"type":"string"},"coverGuids":{"type":"array","items":{"type":"string"}},
  "contactToleranceM":{"type":"number","minimum":0,"maximum":0.001},
   "regionFaceCount":{"type":"integer","minimum":1},"selectedTriangleCount":{"type":"integer","minimum":0},
   "coverBodyCount":{"type":"integer","minimum":0},
  "regionElementGuids":{"type":"array","minItems":1,"items":{"type":"string"}},
  "selectedAreaM2":{"type":"number","minimum":0},"overlayPublished":{"type":"boolean"},
  "nativeComparisonAvailable":{"type":"boolean"},"nativeComparisonNote":{"type":"string"},
  "nativeSurfaces":{"type":"array","items":{"type":"object","additionalProperties":false,
    "properties":{"surfaceAttributeIndex":{"type":"integer","minimum":1},"areaM2":{"type":"number","minimum":0}},"required":["surfaceAttributeIndex","areaM2"]}},
  "surfaces":{"type":"array","items":{"type":"object","additionalProperties":false,
    "properties":{"surfaceAttributeIndex":{"type":"integer","minimum":1},"name":{"type":"string"},"rgba":{"type":"integer","minimum":0},"areaM2":{"type":"number","minimum":0}},"required":["surfaceAttributeIndex","name","rgba","areaM2"]}},
  "meshes":{"type":"array","items":{"type":"object","additionalProperties":false,"properties":{
    "guid":{"type":"string"},"sourceBody":{"type":"integer","minimum":1},"sourcePgon":{"type":"integer","minimum":1},
    "surfaceAttributeIndex":{"type":"integer","minimum":1},"areaM2":{"type":"number","exclusiveMinimum":0},
    "points":{"type":"array","minItems":9,"items":{"type":"number"}},"triangles":{"type":"array","minItems":3,"items":{"type":"integer","minimum":0}}
  },"required":["guid","sourceBody","sourcePgon","surfaceAttributeIndex","areaM2","points","triangles"]}}
  },"required":["snapshotId","captureStamp","classification","unit","coverScopeId","coverGuids","contactToleranceM","regionFaceCount","coverBodyCount","regionElementGuids","selectedTriangleCount","selectedAreaM2","overlayPublished","nativeComparisonAvailable","nativeComparisonNote","nativeSurfaces","surfaces","meshes"]
})json";
const char kEmptyInput[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";
const char kClearInput[] = R"json({
 "type":"object","additionalProperties":false,
 "properties":{"generation":{"type":"integer","minimum":1}}
})json";
const char kClearOutput[] =
    R"json({"type":"object","properties":{"cleared":{"type":"boolean"}},"required":["cleared"],"additionalProperties":false})json";

surfacetakeoff::Request FromObjectState (const GS::ObjectState& params)
{
    surfacetakeoff::Request request;
    GS::Int64 id = 0, stamp = 0;
    params.Get ("snapshotId", id);
    params.Get ("captureStamp", stamp);
    request.snapshotId = id;
    request.captureStamp = stamp;
    GS::Array<GS::UniString> strings;
    if (params.Get ("targetGuids", strings))
        for (const auto& value : strings)
            request.targetGuids.push_back (Utf8 (value));
    strings.Clear ();
    if (params.Get ("coverGuids", strings))
        for (const auto& value : strings)
            request.coverGuids.push_back (Utf8 (value));
    GS::UniString scope, mode;
    params.Get ("coverScopeId", scope);
    params.Get ("regionMode", mode);
    request.coverScopeId = Utf8 (scope);
    request.regionMode = Utf8 (mode);
    if (params.Contains ("contactToleranceM")) {
        double tolerance = 0;
        if (ReadReal (params, "contactToleranceM", tolerance))
            request.contactToleranceM = tolerance;
    }
    GS::Array<GS::ObjectState> seeds;
    if (params.Get ("seeds", seeds))
        for (const auto& row : seeds) {
            GS::UniString guid;
            row.Get ("guid", guid);
            surfacetakeoff::Seed seed;
            seed.guid = Utf8 (guid);
            ReadReals (row, "point", seed.point);
            ReadReals (row, "normal", seed.normal);
            request.seeds.push_back (std::move (seed));
        }
    GS::Array<GS::Int32> indices;
    if (params.Get ("surfaceAttributeIndices", indices))
        request.surfaceAttributeIndices.assign (indices.Begin (), indices.End ());
    if (params.Contains ("surfaceColors")) {
        GS::Array<GS::Int64> colors;
        if (params.Get ("surfaceColors", colors)) {
            request.surfaceColors.emplace ();
            for (const GS::Int64 color : colors)
                request.surfaceColors->push_back (uint32_t (color));
        }
    }
    params.Get ("publishOverlay", request.publishOverlay);
    params.Get ("compareNative", request.compareNative);
    return request;
}

class SurfaceTakeoffCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "CalculateExteriorSurfaceTakeoff";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    surfacetakeoff::Calculation Calculate (const surfacetakeoff::Request& input, GS::ProcessControl* pc = nullptr) const
    {
        const auto fail = [] (const std::string& error) { return surfacetakeoff::Calculation { std::nullopt, error }; };
        const uint64_t id = input.snapshotId, stamp = input.captureStamp;
        const auto snapshot = MeshStore::Get ().Current ();
        if (!Current (snapshot) || id == 0 || stamp == 0 || snapshot->id != id || snapshot->captureStamp != stamp)
            return fail ("Snapshot missing or stale; prepare a fresh capture.");
        const std::string mode = input.regionMode.empty () ? "seededFacade" : input.regionMode;
        const bool publish = input.publishOverlay;
        // Controller workers never construct SDK quantity jobs or rendezvous on
        // the gate. Publish finalizes that separate comparison on the main thread.
        const bool compare = input.compareNative && !t_controllerExecution;
        const bool allWalls = mode == "allWallFaces";
        const bool allElements = mode == "allElementFaces";
        const bool wholeTargets = allWalls || allElements;
        if (!wholeTargets && mode != "seededFacade")
            return fail ("Unsupported wall region mode.");
        if (input.targetGuids.empty () || input.targetGuids.size () > 64 || input.coverGuids.size () > 128 ||
            (wholeTargets ? !input.seeds.empty () : input.seeds.empty ()) || input.seeds.size () > 8 ||
            input.surfaceAttributeIndices.size () > view::kMaxSurfaces || input.coverScopeId.empty ())
            return fail ("Explicit bounded scopes, surfaces and exterior seeds are required.");
        std::vector<size_t> targetIndices;
        std::vector<ex::ExteriorSeed> seeds;
        ex::Request request;
        request.snapshotId = snapshot->id;
        request.captureStamp = snapshot->captureStamp;
        request.coverScopeId = input.coverScopeId;
        request.contactToleranceM = input.contactToleranceM.value_or (.001);
        for (const auto& guid : input.targetGuids) {
            size_t index = 0;
            if (!FindMesh (*snapshot, guid, index))
                return fail ("Target missing or ambiguous in capture.");
            if (allWalls && snapshot->meshes[index].elemType != 1)
                return fail ("All-wall targets must be captured ModelerAPI walls.");
            if (allElements) {
                const auto type = snapshot->meshes[index].elemType;
                if (type != 1 && type != 2 && type != 17 && type != 19)
                    return fail ("Criteria candidates must be captured walls, slabs, columns or beams.");
            }
            targetIndices.push_back (index);
        }
        std::set<std::string> coverNames;
        for (const auto& guid : input.coverGuids) {
            size_t index = 0;
            if (!FindMesh (*snapshot, guid, index))
                return fail ("Explicit cover missing or ambiguous; no covers omitted.");
            coverNames.insert (Guid (snapshot->meshes[index].guid));
            const std::set<int32_t> bodies (snapshot->meshes[index].triSourceBody.begin (),
                                            snapshot->meshes[index].triSourceBody.end ());
            if (bodies.empty ())
                return fail ("Cover has no source bodies.");
            for (int32_t body : bodies)
                request.covers.push_back ({ index, body });
        }
        for (const auto& inputSeed : input.seeds) {
            ex::ExteriorSeed seed;
            if (!FindMesh (*snapshot, inputSeed.guid, seed.meshIndex) || inputSeed.point.size () != 3 ||
                inputSeed.normal.size () != 3)
                return fail ("Seed requires an exact captured element, world point and callback normal.");
            std::copy (inputSeed.point.begin (), inputSeed.point.end (), seed.point.begin ());
            std::copy (inputSeed.normal.begin (), inputSeed.normal.end (), seed.normal.begin ());
            seeds.push_back (seed);
        }
        const uint64_t generation = pc && !t_controllerExecution && evp::RunCancel::Get ().IsRunning ()
                                        ? evp::RunCancel::Get ().Generation ()
                                        : 0;
        const uint64_t ticket = publish ? view::BeginRequest () : 0;
        const auto cancelled = [&] {
            return (pc && pc->TestBreak ()) || (!t_controllerExecution && RunEnded (generation)) ||
                   !Current (snapshot) || (t_externalCancel && (*t_externalCancel) ());
        };
        std::string error;
        if (!(wholeTargets
                  ? ex::ResolveAllTargetFaces (*snapshot, targetIndices, request.faces, error, {}, cancelled)
                  : ex::ResolveFacadeRegion (*snapshot, targetIndices, seeds, request.faces, error, {}, cancelled)))
            return fail (error);
        std::vector<int32_t> surfaces = input.surfaceAttributeIndices;
        ex::SurfaceSelection selection;
        if (surfaces.empty ()) {
            if (!ex::ResolveSurfaceSelection (*snapshot, request.faces, std::nullopt, selection, error))
                return fail (error);
            std::set<int32_t> captured;
            for (const auto& entry : selection.poolToSurface) {
                if (entry.second <= 0)
                    return fail ("A wall face has no captured original project surface.");
                captured.insert (entry.second);
            }
            if (captured.empty () || captured.size () > view::kMaxSurfaces)
                return fail ("The criteria capture requires 1..512 candidate surfaces.");
            surfaces.assign (captured.begin (), captured.end ());
        }
        std::vector<uint32_t> colors;
        for (size_t i = 0; i < surfaces.size (); ++i)
            colors.push_back (view::DefaultColor (i));
        if (input.surfaceColors) {
            const auto& values = *input.surfaceColors;
            if (!values.empty () && values.size () != surfaces.size ())
                return fail ("One explicit color is required for each selected surface.");
            if (!values.empty ())
                colors.assign (values.begin (), values.end ());
        }
        if (!ex::ResolveSurfaceSelection (*snapshot, request.faces,
                                          std::set<int32_t> (surfaces.begin (), surfaces.end ()), selection, error))
            return fail (error);
        request.materialPoolFilter = selection.materialPoolFilter;
        ex::Result result;
        ex::Limits limits;
        // A library opening contains many separate casing/frame/hardware bodies.
        // Only the body count grows; input/work/time and strict solids stay bounded.
        limits.maxCoverBodies = 2048;
        if (!ex::Calculate (*snapshot, request, result, error, limits, cancelled))
            return fail (error);
        std::map<int32_t, double> nativeAreas;
        std::string note = input.compareNative && !compare
                               ? "Native whole-element comparison is finalized by the main-thread publish command."
                               : "Native comparison not requested. Surface Schedule acceptance is separate.";
        bool nativeAvailable = false;
        if (compare) {
            GS::ObjectState quantityParams;
            GS::Array<GS::ObjectState> elements, coverElements;
            for (const auto& entry : result.byElementGuid)
                elements.Push (Element (entry.first));
            for (const auto& guid : coverNames)
                coverElements.Push (Element (guid));
            quantityParams.Add ("elements", elements);
            quantityParams.Add ("coverElements", coverElements);
            auto quantity = std::make_shared<NativeCommandResult> ();
            GS::UniString gateError;
            if (!evp::MainThreadGate::Get ().Invoke (
                    [snapshot, quantity, quantityParams] {
                        if (Current (snapshot))
                            *quantity = ExecuteNativeCommand ("GetExposedSurfaceQuantities", quantityParams);
                    },
                    evp::MainThreadGate::DefaultTimeoutMs, gateError))
                return fail (Utf8 (gateError));
            if (cancelled ())
                return fail ("Capture changed during native quantity comparison.");
            nativeAvailable = quantity->ok;
            note = quantity->ok
                       ? (wholeTargets
                              ? "Native whole candidate elements, same cover GUIDs. Surface Schedule equivalence not "
                                "established."
                              : "Native whole region elements, same cover GUIDs; NOT equivalent facade-face/schedule "
                                "scope.")
                       : "Native whole-element comparison unavailable: " + Utf8 (quantity->error);
            if (quantity->ok) {
                for (int32_t surface : surfaces)
                    nativeAreas[surface] = 0;
                GS::Array<GS::ObjectState> rows;
                quantity->data.Get ("elements", rows);
                for (const auto& row : rows) {
                    GS::Array<GS::ObjectState> values;
                    row.Get ("quantities", values);
                    for (const auto& value : values) {
                        GS::UniString material;
                        double area = 0;
                        if (!value.Get ("materialIndex", material) || !ReadReal (value, "exposedSurface", area) ||
                            !std::isfinite (area) || area < 0)
                            return fail ("Invalid native comparison row.");
                        try {
                            const int index = std::stoi (Utf8 (material));
                            if (nativeAreas.contains (index))
                                nativeAreas[index] += area;
                        }
                        catch (...) {
                            return fail ("Invalid native surface index.");
                        }
                    }
                }
            }
        }
        note += " Native comparison does not apply the near-contact tolerance or face-direction filters.";
        auto presentation = std::make_shared<view::Presentation> ();
        if (!view::Build (*snapshot, result, selection, surfaces, nativeAreas, note, *presentation, error, colors,
                          wholeTargets))
            return fail (error);
        if (cancelled ())
            return fail ("Capture changed before presentation.");
        if (publish) {
            auto publication = std::make_shared<std::string> ();
            GS::UniString gateError;
            if (!evp::MainThreadGate::Get ().Invoke (
                    [snapshot, targetIndices, presentation] {
                        if (Current (snapshot))
                            ReadSurfaceCriteriaMetadata (*snapshot, targetIndices, presentation->elements);
                    },
                    evp::MainThreadGate::DefaultTimeoutMs, gateError))
                return fail (Utf8 (gateError));
            if (allElements)
                presentation->scopeLabel = "Criteria candidates: captured walls, slabs, columns and beams. Includes "
                                           "interiors; not exterior-only.";
            if (!evp::MainThreadGate::Get ().Invoke (
                    [snapshot, presentation, publication, generation, ticket] {
                        if (!Current (snapshot) || RunEnded (generation))
                            *publication = "Capture changed or command cancelled before publication.";
                        else
                            view::Publish (*presentation, snapshot->id, snapshot->captureStamp, ticket, *publication);
                    },
                    evp::MainThreadGate::DefaultTimeoutMs, gateError))
                return fail (Utf8 (gateError));
            if (!publication->empty ())
                return fail (*publication);
        }
        surfacetakeoff::Result output;
        output.snapshotId = id;
        output.captureStamp = stamp;
        output.classification = allElements ? "allElementFaces-contactOnly"
                                : allWalls  ? "allWallFaces-contactOnly"
                                            : "userExteriorSeeds-connectedCoplanar";
        output.unit = "m2";
        output.coverScopeId = input.coverScopeId;
        output.contactToleranceM = result.contactToleranceM;
        output.coverGuids.assign (coverNames.begin (), coverNames.end ());
        output.regionFaceCount = request.faces.size ();
        output.coverBodyCount = result.covers.size ();
        for (const auto& entry : result.byElementGuid)
            output.regionElementGuids.push_back (entry.first);
        for (const size_t index : targetIndices)
            output.targetGuids.push_back (snapshot->meshes[index].guid);
        output.selectedTriangleCount = presentation->triangles;
        output.selectedAreaM2 = presentation->areaM2;
        output.overlayPublished = publish;
        output.nativeComparisonAvailable = nativeAvailable;
        output.nativeComparisonRequested = input.compareNative;
        output.nativeComparisonNote = note;
        for (const auto& row : presentation->rows)
            output.surfaces.push_back ({ row.index, row.name, row.rgba, row.areaM2 });
        for (const auto& [index, area] : nativeAreas)
            output.nativeSurfaces.push_back ({ index, area });
        for (const auto& fragment : result.fragments)
            if (fragment.selected) {
                surfacetakeoff::FragmentMesh mesh;
                mesh.guid = snapshot->meshes[fragment.source.body.meshIndex].guid;
                mesh.sourceBody = fragment.source.body.sourceBody;
                mesh.sourcePgon = fragment.source.sourcePgon;
                mesh.surfaceAttributeIndex = selection.poolToSurface.at (fragment.materialPoolIndex);
                mesh.areaM2 = fragment.areaM2;
                mesh.points = fragment.points;
                mesh.triangles = fragment.triangles;
                output.meshes.push_back (std::move (mesh));
            }
        // Publication is the commit point: a later Stop does not turn a valid
        // completed display into a false partial-result failure. Capture drift
        // always refuses (the native freshness timer removes its old geometry).
        if (!Current (snapshot) || (!publish && cancelled ()))
            return fail ("Capture changed during response serialization.");
        return { std::move (output), {} };
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl& pc) const override;
};

NativeCommandResult ToNativeResult (const surfacetakeoff::Result& value)
{
    GS::ObjectState output;
    output.Add ("snapshotId", GS::Int64 (value.snapshotId));
    output.Add ("captureStamp", GS::Int64 (value.captureStamp));
    output.Add ("classification", Text (value.classification));
    output.Add ("unit", Text (value.unit));
    output.Add ("coverScopeId", Text (value.coverScopeId));
    output.Add ("contactToleranceM", value.contactToleranceM);
    GS::Array<GS::UniString> coverGuids, regionElementGuids;
    for (const auto& guid : value.coverGuids)
        coverGuids.Push (Text (guid));
    for (const auto& guid : value.regionElementGuids)
        regionElementGuids.Push (Text (guid));
    output.Add ("coverGuids", coverGuids);
    output.Add ("regionFaceCount", GS::Int64 (value.regionFaceCount));
    output.Add ("coverBodyCount", GS::Int64 (value.coverBodyCount));
    output.Add ("regionElementGuids", regionElementGuids);
    output.Add ("selectedTriangleCount", GS::Int64 (value.selectedTriangleCount));
    output.Add ("selectedAreaM2", value.selectedAreaM2);
    output.Add ("overlayPublished", value.overlayPublished);
    output.Add ("nativeComparisonAvailable", value.nativeComparisonAvailable);
    output.Add ("nativeComparisonNote", Text (value.nativeComparisonNote));
    GS::Array<GS::ObjectState> surfaceRows, meshes, nativeRows;
    for (const auto& row : value.surfaces) {
        GS::ObjectState item;
        item.Add ("surfaceAttributeIndex", row.surfaceAttributeIndex);
        item.Add ("name", Text (row.name));
        item.Add ("rgba", GS::Int64 (row.rgba));
        item.Add ("areaM2", row.areaM2);
        surfaceRows.Push (item);
    }
    for (const auto& row : value.nativeSurfaces) {
        GS::ObjectState item;
        item.Add ("surfaceAttributeIndex", row.surfaceAttributeIndex);
        item.Add ("areaM2", row.areaM2);
        nativeRows.Push (item);
    }
    for (const auto& mesh : value.meshes) {
        GS::ObjectState item;
        item.Add ("guid", Text (mesh.guid));
        item.Add ("sourceBody", mesh.sourceBody);
        item.Add ("sourcePgon", mesh.sourcePgon);
        item.Add ("surfaceAttributeIndex", mesh.surfaceAttributeIndex);
        item.Add ("areaM2", mesh.areaM2);
        item.Add ("points", Array (mesh.points));
        item.Add ("triangles", Array (mesh.triangles));
        meshes.Push (item);
    }
    output.Add ("surfaces", surfaceRows);
    output.Add ("meshes", meshes);
    output.Add ("nativeSurfaces", nativeRows);
    return output;
}

bool FinalizeNativeComparison (surfacetakeoff::Result& result, std::string& error)
{
    if (!result.nativeComparisonRequested)
        return true;
    GS::ObjectState params;
    GS::Array<GS::ObjectState> elements, covers;
    for (const auto& guid : result.regionElementGuids)
        elements.Push (Element (guid));
    for (const auto& guid : result.coverGuids)
        covers.Push (Element (guid));
    params.Add ("elements", elements);
    params.Add ("coverElements", covers);
    // Main-thread publication/finalization only: no MainThreadGate wait and no
    // SDK/GS object is captured by the detached controller worker.
    const auto quantity = ExecuteNativeCommand ("GetExposedSurfaceQuantities", params);
    const bool wholeTargets = result.classification != "userExteriorSeeds-connectedCoplanar";
    result.nativeComparisonAvailable = quantity.ok;
    result.nativeComparisonNote =
        quantity.ok ? (wholeTargets ? "Native whole candidate elements, same cover GUIDs. Surface Schedule equivalence "
                                      "not established."
                                    : "Native whole region elements, same cover GUIDs; NOT equivalent facade-face/"
                                      "schedule scope.")
                    : "Native whole-element comparison unavailable: " + Utf8 (quantity.error);
    result.nativeSurfaces.clear ();
    if (quantity.ok) {
        std::map<int32_t, double> areas;
        for (const auto& row : result.surfaces)
            areas[row.surfaceAttributeIndex] = 0;
        GS::Array<GS::ObjectState> rows;
        quantity.data.Get ("elements", rows);
        for (const auto& row : rows) {
            GS::Array<GS::ObjectState> values;
            row.Get ("quantities", values);
            for (const auto& value : values) {
                GS::UniString material;
                double area = 0;
                if (!value.Get ("materialIndex", material) || !ReadReal (value, "exposedSurface", area) ||
                    !std::isfinite (area) || area < 0) {
                    error = "Invalid native comparison row.";
                    return false;
                }
                try {
                    const int index = std::stoi (Utf8 (material));
                    if (areas.contains (index))
                        areas[index] += area;
                }
                catch (...) {
                    error = "Invalid native surface index.";
                    return false;
                }
            }
        }
        for (const auto& [index, area] : areas)
            result.nativeSurfaces.push_back ({ index, area });
    }
    result.nativeComparisonNote +=
        " Native comparison does not apply the near-contact tolerance or face-direction filters.";
    return true;
}

NativeCommandResult PublishResult (const surfacetakeoff::Result& result, uint64_t generation)
{
    const auto snapshot = MeshStore::Get ().Current ();
    if (!Current (snapshot) || snapshot->id != result.snapshotId || snapshot->captureStamp != result.captureStamp)
        return NativeCommandResult::Failure (Text ("Capture changed before controller overlay publication."));
    surfacetakeoff::Result finalized = result;
    std::string error;
    if (!FinalizeNativeComparison (finalized, error))
        return NativeCommandResult::Failure (Text (error));
    view::Presentation presentation;
    if (!view::BuildFromResult (*snapshot, finalized, presentation, error))
        return NativeCommandResult::Failure (Text (error));
    std::vector<size_t> targets;
    for (const auto& guid : finalized.targetGuids) {
        size_t index = 0;
        if (!FindMesh (*snapshot, guid, index))
            return NativeCommandResult::Failure (Text ("Published target is missing or ambiguous in the capture."));
        targets.push_back (index);
    }
    ReadSurfaceCriteriaMetadata (*snapshot, targets, presentation.elements);
    const uint64_t ticket = view::BeginRequest ();
    if (!view::Publish (presentation, finalized.snapshotId, finalized.captureStamp, ticket, error))
        return NativeCommandResult::Failure (Text (error));
    finalized.overlayPublished = true;
    GS::ObjectState output;
    output.Add ("overlayPublished", true);
    output.Add ("generation", GS::Int64 (generation));
    output.Add ("snapshotId", GS::Int64 (finalized.snapshotId));
    output.Add ("captureStamp", GS::Int64 (finalized.captureStamp));
    output.Add ("result", ToNativeResult (finalized).data);
    return output;
}

NativeCommandResult SurfaceTakeoffCommand::ExecuteNative (const GS::ObjectState& params, GS::ProcessControl& pc) const
{
    const auto calculation = Calculate (FromObjectState (params), &pc);
    if (!calculation.result)
        return NativeCommandResult::Failure (
            EVP_FAIL (Text (calculation.error), "Tapioca.CalculateExteriorSurfaceTakeoff"));
    return ToNativeResult (*calculation.result);
}

struct ExternalCancelScope {
    explicit ExternalCancelScope (const std::function<bool ()>& cancelled)
        : previousCancel (t_externalCancel), previousController (t_controllerExecution)
    {
        t_controllerExecution = static_cast<bool> (cancelled);
        t_externalCancel = t_controllerExecution ? &cancelled : nullptr;
    }
    ~ExternalCancelScope ()
    {
        t_externalCancel = previousCancel;
        t_controllerExecution = previousController;
    }
    const std::function<bool ()>* previousCancel;
    bool previousController;
};

} // namespace

const char* SurfaceTakeoffInputSchema ()
{
    return kInput;
}

surfacetakeoff::Request SurfaceTakeoffRequestFromNative (const GS::ObjectState& params)
{
    return FromObjectState (params);
}

surfacetakeoff::Calculation CalculateExteriorSurfaceResult (const surfacetakeoff::Request& request,
                                                            const std::function<bool ()>& cancelled)
{
    ExternalCancelScope scope (cancelled);
    SurfaceTakeoffCommand command;
    return command.Calculate (request);
}

surfacetakeoff::Calculation CalculateExteriorSurfaceResult (const GS::ObjectState& params,
                                                            GS::ProcessControl& processControl,
                                                            const std::function<bool ()>& cancelled)
{
    ExternalCancelScope scope (cancelled);
    SurfaceTakeoffCommand command;
    return command.Calculate (FromObjectState (params), &processControl);
}

NativeCommandResult ToNativeSurfaceTakeoffResult (const surfacetakeoff::Result& result)
{
    return ToNativeResult (result);
}

NativeCommandResult PublishExteriorSurfaceResult (const surfacetakeoff::Result& result, uint64_t generation)
{
    return PublishResult (result, generation);
}

NativeCommandResult CalculateExteriorSurfaceTakeoff (const GS::ObjectState& params, GS::ProcessControl& processControl,
                                                     const std::function<bool ()>& cancelled)
{
    ExternalCancelScope scope (cancelled);
    SurfaceTakeoffCommand command;
    return command.ExecuteNative (params, processControl);
}

namespace {
class ClearSurfaceTakeoffCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ClearExteriorSurfaceTakeoff";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 generation = 0;
        params.Get ("generation", generation);
        const bool cleared = geomsrv::surfacetakeoff::SharedSurfaceTakeoffController ().Clear (uint64_t (generation));
        if (cleared)
            view::Clear ();
        GS::ObjectState output;
        output.Add ("cleared", cleared);
        return output;
    }
};
const NativeCommandRegistration kRegistrations[] = {
    { "CalculateExteriorSurfaceTakeoff", &MakeRegisteredNativeCommand<SurfaceTakeoffCommand>, false, kInput, kOutput },
    { "ClearExteriorSurfaceTakeoff", &MakeRegisteredNativeCommand<ClearSurfaceTakeoffCommand>, true, kClearInput,
      kClearOutput }
};
} // namespace
NativeCommandRegistrations GetSurfaceTakeoffCommandRegistrations ()
{
    return MakeRegistrationView (kRegistrations);
}
} // namespace geomsrv
