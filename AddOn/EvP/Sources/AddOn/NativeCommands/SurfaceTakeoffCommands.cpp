// OVERLAY-INVARIANTS.md: seeded facade calculation, existing layer publication.
#include "NativeCommands/SurfaceTakeoffCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "NativeCommands/SurfaceCriteriaMetadata.hpp"
#include "AddOnCommands.hpp"
#include "ArchViz/SurfaceTakeoffOverlay.hpp"
#include "Geometry/ExteriorSurfaceRegion.hpp"
#include "Geometry/MeshStore.hpp"
#include "Python/MainThreadGate.hpp"
#include "Python/RunCancel.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace geomsrv {
namespace {
namespace ex = exposed;
namespace view = archviz::surfacetakeoff;
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
bool FindMesh (const Snapshot& snapshot, const GS::UniString& guid, size_t& index)
{
    size_t matches = 0;
    for (size_t i = 0; i < snapshot.meshes.size (); ++i)
        if (Guid (snapshot.meshes[i].guid) == Guid (Utf8 (guid))) {
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
  "surfaceAttributeIndices":{"type":"array","maxItems":64,"uniqueItems":true,"items":{"type":"integer","minimum":1}},
  "surfaceColors":{"type":"array","maxItems":64,"items":{"type":"integer","minimum":0,"maximum":4294967295}},
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
 },"required":["snapshotId","captureStamp","classification","unit","coverScopeId","coverGuids","contactToleranceM","regionFaceCount","regionElementGuids","selectedTriangleCount","selectedAreaM2","overlayPublished","nativeComparisonAvailable","nativeComparisonNote","nativeSurfaces","surfaces","meshes"]
})json";
const char kEmptyInput[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";
const char kClearOutput[] =
    R"json({"type":"object","properties":{"cleared":{"type":"boolean"}},"required":["cleared"],"additionalProperties":false})json";

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
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl& pc) const override
    {
        const auto fail = [] (const std::string& error) {
            return NativeCommandResult::Failure (EVP_FAIL (Text (error), "Tapioca.CalculateExteriorSurfaceTakeoff"));
        };
        GS::Int64 id = 0, stamp = 0;
        params.Get ("snapshotId", id);
        params.Get ("captureStamp", stamp);
        const auto snapshot = MeshStore::Get ().Current ();
        if (!Current (snapshot) || id <= 0 || stamp <= 0 || snapshot->id != uint64_t (id) ||
            snapshot->captureStamp != uint64_t (stamp))
            return fail ("Snapshot missing or stale; prepare a fresh capture.");
        GS::Array<GS::UniString> targets, covers;
        GS::Array<GS::ObjectState> inputs;
        GS::Array<GS::Int32> indices;
        GS::UniString scope;
        bool publish = false, compare = false;
        GS::UniString mode ("seededFacade");
        params.Get ("regionMode", mode);
        const bool allWalls = mode == "allWallFaces";
        const bool allElements = mode == "allElementFaces";
        const bool wholeTargets = allWalls || allElements;
        if (!wholeTargets && mode != "seededFacade")
            return fail ("Unsupported wall region mode.");
        if (!params.Get ("targetGuids", targets) || !params.Get ("coverGuids", covers) ||
            !params.Get ("seeds", inputs) || !params.Get ("surfaceAttributeIndices", indices) ||
            !params.Get ("coverScopeId", scope) || !params.Get ("publishOverlay", publish) ||
            !params.Get ("compareNative", compare) || targets.IsEmpty () || targets.GetSize () > 64 ||
            covers.GetSize () > 128 || (wholeTargets ? !inputs.IsEmpty () : inputs.IsEmpty ()) ||
            inputs.GetSize () > 8 || indices.GetSize () > 64 || scope.IsEmpty ())
            return fail ("Explicit bounded scopes, surfaces and exterior seeds are required.");
        std::vector<size_t> targetIndices;
        std::vector<ex::ExteriorSeed> seeds;
        ex::Request request;
        request.snapshotId = snapshot->id;
        request.captureStamp = snapshot->captureStamp;
        request.coverScopeId = Utf8 (scope);
        request.contactToleranceM = .001;
        if (params.Contains ("contactToleranceM") && !ReadReal (params, "contactToleranceM", request.contactToleranceM))
            return fail ("Invalid contact tolerance.");
        for (const auto& guid : targets) {
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
        for (const auto& guid : covers) {
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
        for (const auto& input : inputs) {
            GS::UniString guid;
            std::vector<double> point, normal;
            ex::ExteriorSeed seed;
            if (!input.Get ("guid", guid) || !FindMesh (*snapshot, guid, seed.meshIndex) ||
                !ReadReals (input, "point", point) || !ReadReals (input, "normal", normal) || point.size () != 3 ||
                normal.size () != 3)
                return fail ("Seed requires an exact captured element, world point and callback normal.");
            std::copy (point.begin (), point.end (), seed.point.begin ());
            std::copy (normal.begin (), normal.end (), seed.normal.begin ());
            seeds.push_back (seed);
        }
        const uint64_t generation = evp::RunCancel::Get ().IsRunning () ? evp::RunCancel::Get ().Generation () : 0;
        const uint64_t ticket = publish ? view::BeginRequest () : 0;
        const auto cancelled = [&] { return pc.TestBreak () || RunEnded (generation) || !Current (snapshot); };
        std::string error;
        if (!(wholeTargets
                  ? ex::ResolveAllTargetFaces (*snapshot, targetIndices, request.faces, error, {}, cancelled)
                  : ex::ResolveFacadeRegion (*snapshot, targetIndices, seeds, request.faces, error, {}, cancelled)))
            return fail (error);
        std::vector<int32_t> surfaces (indices.Begin (), indices.End ());
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
            if (captured.empty () || captured.size () > 64)
                return fail ("The material list requires 1..64 captured wall surfaces.");
            surfaces.assign (captured.begin (), captured.end ());
        }
        std::vector<uint32_t> colors;
        for (size_t i = 0; i < surfaces.size (); ++i)
            colors.push_back (view::DefaultColor (i));
        if (params.Contains ("surfaceColors")) {
            GS::Array<GS::Int64> values;
            if (!params.Get ("surfaceColors", values) || (!values.IsEmpty () && values.GetSize () != surfaces.size ()))
                return fail ("One explicit color is required for each selected surface.");
            if (!values.IsEmpty ())
                colors.clear ();
            for (GS::Int64 value : values) {
                if (value < 0 || value > 0xFFFFFFFFLL)
                    return fail ("Invalid surface color.");
                colors.push_back (uint32_t (value));
            }
        }
        if (!ex::ResolveSurfaceSelection (*snapshot, request.faces,
                                          std::set<int32_t> (surfaces.begin (), surfaces.end ()), selection, error))
            return fail (error);
        request.materialPoolFilter = selection.materialPoolFilter;
        ex::Result result;
        if (!ex::Calculate (*snapshot, request, result, error, {}, cancelled))
            return fail (error);
        std::map<int32_t, double> nativeAreas;
        std::string note = "Native comparison not requested. Surface Schedule acceptance is separate.";
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
        GS::ObjectState output;
        output.Add ("snapshotId", id);
        output.Add ("captureStamp", stamp);
        output.Add ("classification", GS::UniString (allElements ? "allElementFaces-contactOnly"
                                                     : allWalls  ? "allWallFaces-contactOnly"
                                                                 : "userExteriorSeeds-connectedCoplanar"));
        output.Add ("unit", GS::UniString ("m2"));
        output.Add ("coverScopeId", scope);
        output.Add ("contactToleranceM", result.contactToleranceM);
        GS::Array<GS::UniString> coverEcho;
        for (const auto& guid : coverNames)
            coverEcho.Push (Text (guid));
        output.Add ("coverGuids", coverEcho);
        output.Add ("regionFaceCount", GS::Int64 (request.faces.size ()));
        GS::Array<GS::UniString> regionElements;
        for (const auto& entry : result.byElementGuid)
            regionElements.Push (Text (entry.first));
        output.Add ("regionElementGuids", regionElements);
        output.Add ("selectedTriangleCount", GS::Int64 (presentation->triangles));
        output.Add ("selectedAreaM2", presentation->areaM2);
        output.Add ("overlayPublished", publish);
        output.Add ("nativeComparisonAvailable", nativeAvailable);
        output.Add ("nativeComparisonNote", Text (note));
        GS::Array<GS::ObjectState> surfaceRows, meshes, nativeRows;
        for (const auto& row : presentation->rows) {
            GS::ObjectState item;
            item.Add ("surfaceAttributeIndex", row.index);
            item.Add ("name", Text (row.name));
            item.Add ("rgba", GS::Int64 (row.rgba));
            item.Add ("areaM2", row.areaM2);
            surfaceRows.Push (item);
        }
        for (const auto& [index, area] : nativeAreas) {
            GS::ObjectState row;
            row.Add ("surfaceAttributeIndex", index);
            row.Add ("areaM2", area);
            nativeRows.Push (row);
        }
        for (const auto& fragment : result.fragments)
            if (fragment.selected) {
                GS::ObjectState mesh;
                mesh.Add ("guid", Text (snapshot->meshes[fragment.source.body.meshIndex].guid));
                mesh.Add ("sourceBody", fragment.source.body.sourceBody);
                mesh.Add ("sourcePgon", fragment.source.sourcePgon);
                mesh.Add ("surfaceAttributeIndex", selection.poolToSurface.at (fragment.materialPoolIndex));
                mesh.Add ("areaM2", fragment.areaM2);
                mesh.Add ("points", Array (fragment.points));
                mesh.Add ("triangles", Array (fragment.triangles));
                meshes.Push (mesh);
            }
        output.Add ("surfaces", surfaceRows);
        output.Add ("meshes", meshes);
        output.Add ("nativeSurfaces", nativeRows);
        // Publication is the commit point: a later Stop does not turn a valid
        // completed display into a false partial-result failure. Capture drift
        // always refuses (the native freshness timer removes its old geometry).
        if (!Current (snapshot) || (!publish && cancelled ()))
            return fail ("Capture changed during response serialization.");
        return output;
    }
};
class ClearSurfaceTakeoffCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "ClearExteriorSurfaceTakeoff";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        view::Clear ();
        GS::ObjectState output;
        output.Add ("cleared", true);
        return output;
    }
};
const NativeCommandRegistration kRegistrations[] = {
    { "CalculateExteriorSurfaceTakeoff", &MakeRegisteredNativeCommand<SurfaceTakeoffCommand>, false, kInput, kOutput },
    { "ClearExteriorSurfaceTakeoff", &MakeRegisteredNativeCommand<ClearSurfaceTakeoffCommand>, false, kEmptyInput,
      kClearOutput }
};
} // namespace
NativeCommandRegistrations GetSurfaceTakeoffCommandRegistrations ()
{
    return MakeRegistrationView (kRegistrations);
}
} // namespace geomsrv
