#include "NativeCommands/SurfaceFragmentCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "Geometry/MeshStore.hpp"
#include "Geometry/SurfaceAttributeMap.hpp"

#include <algorithm>
#include <cctype>

namespace geomsrv {
namespace {
namespace ex = exposed;

constexpr const char kCatalogInput[] = R"json({
    "type":"object","properties":{
        "snapshotId":{"type":"integer","minimum":1},
        "captureStamp":{"type":"integer","minimum":1}
    },"additionalProperties":false,"required":["snapshotId","captureStamp"]
})json";
constexpr const char kCatalogOutput[] = R"json({
    "type":"object","properties":{
        "snapshotId":{"type":"integer","minimum":1},
        "captureStamp":{"type":"integer","minimum":1},
        "scope":{"const":"capturedProjectSurfaceCatalog"},
        "surfaces":{"type":"array","items":{"type":"object","properties":{
            "surfaceAttributeIndex":{"type":"integer","minimum":1},
            "name":{"type":"string"},"guid":{"type":"string"}
        },"additionalProperties":false,"required":["surfaceAttributeIndex","name","guid"]}}
    },"additionalProperties":false,"required":["snapshotId","captureStamp","scope","surfaces"]
})json";
constexpr const char kCalculateInput[] = R"json({
    "type":"object","properties":{
        "snapshotId":{"type":"integer","minimum":1},
        "captureStamp":{"type":"integer","minimum":1},
        "coverScopeId":{"type":"string","minLength":1,"maxLength":256},
        "contactToleranceM":{"type":"number","minimum":0,"maximum":0.001},
        "faces":{"type":"array","minItems":1,"maxItems":512,"items":{"$ref":"#/$defs/face"}},
        "covers":{"type":"array","maxItems":128,"items":{"$ref":"#/$defs/body"}},
        "surfaceAttributeIndices":{"type":"array","maxItems":512,"uniqueItems":true,"items":{"type":"integer","minimum":1}}
    },"additionalProperties":false,"required":["snapshotId","captureStamp","coverScopeId","faces","covers"],
    "$defs":{
        "elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1,"maxLength":38}},"additionalProperties":false,"required":["guid"]},
        "body":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"sourceBody":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["elementId","sourceBody"]},
        "face":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"sourceBody":{"type":"integer","minimum":1},"sourcePgon":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["elementId","sourceBody","sourcePgon"]}
    }
})json";
constexpr const char kCalculateOutput[] = R"json({
    "type":"object","properties":{
        "snapshotId":{"type":"integer","minimum":1},"captureStamp":{"type":"integer","minimum":1},
        "algorithmVersion":{"type":"integer","minimum":1},"coverScopeId":{"type":"string","minLength":1},
        "contactToleranceM":{"type":"number","minimum":0,"maximum":0.001},
        "unit":{"const":"m2"},"coordinateUnit":{"const":"m"},"indexSpace":{"const":"ModelerAPI-1-based"},
        "classification":{"const":"explicitSourceFaces-contactOnly"},
        "areas":{"$ref":"#/$defs/areas"},
        "covers":{"type":"array","items":{"$ref":"#/$defs/body"}},
        "faces":{"type":"array","items":{"$ref":"#/$defs/faceAreas"}},
        "fragments":{"type":"array","items":{"$ref":"#/$defs/fragment"}},
        "surfaces":{"type":"array","items":{"type":"object","properties":{
            "surfaceAttributeIndex":{"type":"integer","minimum":0},"areas":{"$ref":"#/$defs/areas"}
        },"additionalProperties":false,"required":["surfaceAttributeIndex","areas"]}},
        "elements":{"type":"array","items":{"type":"object","properties":{
            "elementId":{"$ref":"#/$defs/elementId"},"areas":{"$ref":"#/$defs/areas"}
        },"additionalProperties":false,"required":["elementId","areas"]}},
        "statistics":{"type":"object","properties":{
            "candidateCoverPairs":{"$ref":"#/$defs/count"},"sectionCount":{"$ref":"#/$defs/count"},
            "work":{"$ref":"#/$defs/count"},"outputTriangles":{"$ref":"#/$defs/count"}
        },"additionalProperties":false,"required":["candidateCoverPairs","sectionCount","work","outputTriangles"]}
    },"additionalProperties":false,
    "required":["snapshotId","captureStamp","algorithmVersion","coverScopeId","contactToleranceM","unit","coordinateUnit","indexSpace","classification","areas","covers","faces","fragments","surfaces","elements","statistics"],
    "$defs":{
        "count":{"type":"integer","minimum":0},
        "elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},
        "areas":{"type":"object","properties":{
            "grossGeometricArea":{"type":"number","minimum":0},"exposedArea":{"type":"number","minimum":0},"selectedExposedArea":{"type":"number","minimum":0}
        },"additionalProperties":false,"required":["grossGeometricArea","exposedArea","selectedExposedArea"]},
        "body":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"sourceBody":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["elementId","sourceBody"]},
        "source":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"sourceBody":{"type":"integer","minimum":1},"sourcePgon":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["elementId","sourceBody","sourcePgon"]},
        "faceAreas":{"type":"object","properties":{
            "source":{"$ref":"#/$defs/source"},"materialPoolIndex":{"type":"integer","minimum":1},
            "surfaceAttributeIndex":{"type":"integer","minimum":0},"areas":{"$ref":"#/$defs/areas"}
        },"additionalProperties":false,"required":["source","materialPoolIndex","surfaceAttributeIndex","areas"]},
        "fragment":{"type":"object","properties":{
            "source":{"$ref":"#/$defs/source"},"materialPoolIndex":{"type":"integer","minimum":1},
            "surfaceAttributeIndex":{"type":"integer","minimum":0},"selected":{"type":"boolean"},
            "areaM2":{"type":"number","exclusiveMinimum":0},
            "loops":{"type":"array","minItems":1,"items":{"type":"array","minItems":9,"items":{"type":"number"}}},
            "points":{"type":"array","minItems":9,"items":{"type":"number"}},
            "triangles":{"type":"array","minItems":3,"items":{"type":"integer","minimum":0}}
        },"additionalProperties":false,"required":["source","materialPoolIndex","surfaceAttributeIndex","selected","areaM2","loops","points","triangles"]}
    }
})json";

GS::UniString Text (const std::string& value)
{
    return GS::UniString (value.c_str (), CC_UTF8);
}

GS::ObjectState ElementId (const std::string& guid)
{
    GS::ObjectState id;
    id.Add ("guid", Text (guid));
    return id;
}

std::string CanonicalGuid (std::string guid)
{
    if (guid.size () == 38 && guid.front () == '{' && guid.back () == '}')
        guid = guid.substr (1, 36);
    std::transform (guid.begin (), guid.end (), guid.begin (),
                    [] (unsigned char c) { return static_cast<char> (std::toupper (c)); });
    return guid;
}

bool IsCurrent (const std::shared_ptr<const Snapshot>& snapshot)
{
    return snapshot && MeshStore::Get ().Current () == snapshot &&
           MeshStore::Get ().CaptureStamp () == snapshot->captureStamp;
}

std::shared_ptr<const Snapshot> Pin (const GS::ObjectState& params)
{
    GS::Int64 id = 0, stamp = 0;
    if (!params.Get ("snapshotId", id) || !params.Get ("captureStamp", stamp) || id <= 0 || stamp <= 0)
        return nullptr;
    const auto snapshot = MeshStore::Get ().Current ();
    if (!IsCurrent (snapshot) || snapshot->id != static_cast<uint64_t> (id) ||
        snapshot->captureStamp != static_cast<uint64_t> (stamp))
        return nullptr;
    return snapshot;
}

bool ResolveBody (const Snapshot& snapshot, const GS::ObjectState& input, ex::BodyKey& key)
{
    GS::ObjectState id;
    GS::UniString guid;
    GS::Int32 body = 0;
    if (!input.Get ("elementId", id) || !id.Get ("guid", guid) || guid.IsEmpty () || !input.Get ("sourceBody", body) ||
        body <= 0)
        return false;
    const auto wanted = CanonicalGuid (guid.ToCStr (0, MaxUSize, CC_UTF8).Get ());
    bool found = false;
    for (size_t i = 0; i < snapshot.meshes.size (); ++i) {
        if (CanonicalGuid (snapshot.meshes[i].guid) != wanted)
            continue;
        if (found)
            return false;
        found = true;
        key = { i, body };
    }
    return found;
}

GS::ObjectState DescribeBody (const Snapshot& snapshot, const ex::BodyKey& key)
{
    GS::ObjectState row;
    row.Add ("elementId", ElementId (snapshot.meshes[key.meshIndex].guid));
    row.Add ("sourceBody", key.sourceBody);
    return row;
}

GS::ObjectState DescribeSource (const Snapshot& snapshot, const ex::FaceKey& key)
{
    auto row = DescribeBody (snapshot, key.body);
    row.Add ("sourcePgon", key.sourcePgon);
    return row;
}

GS::ObjectState DescribeAreas (const ex::Areas& areas)
{
    GS::ObjectState row;
    row.Add ("grossGeometricArea", areas.grossGeometricArea);
    row.Add ("exposedArea", areas.exposedArea);
    row.Add ("selectedExposedArea", areas.selectedExposedArea);
    return row;
}

template <typename T> GS::Array<T> Array (const std::vector<T>& input)
{
    GS::Array<T> output;
    for (const auto& value : input)
        output.Push (value);
    return output;
}

GS::ObjectState DescribeResult (const Snapshot& snapshot, const ex::Result& result,
                                const ex::SurfaceSelection& selection, const std::optional<std::set<int32_t>>& filter)
{
    GS::ObjectState output;
    output.Add ("snapshotId", static_cast<GS::Int64> (result.snapshotId));
    output.Add ("captureStamp", static_cast<GS::Int64> (result.captureStamp));
    output.Add ("algorithmVersion", result.algorithmVersion);
    output.Add ("coverScopeId", Text (result.coverScopeId));
    output.Add ("contactToleranceM", result.contactToleranceM);
    output.Add ("unit", GS::UniString ("m2"));
    output.Add ("coordinateUnit", GS::UniString ("m"));
    output.Add ("indexSpace", GS::UniString ("ModelerAPI-1-based"));
    output.Add ("classification", GS::UniString ("explicitSourceFaces-contactOnly"));
    output.Add ("areas", DescribeAreas (result.areas));
    GS::Array<GS::ObjectState> covers, faces, fragments, surfaces, elements;
    for (const auto& body : result.covers)
        covers.Push (DescribeBody (snapshot, body));
    for (const auto& face : result.faces) {
        GS::ObjectState row;
        row.Add ("source", DescribeSource (snapshot, face.source));
        row.Add ("materialPoolIndex", face.materialPoolIndex);
        row.Add ("surfaceAttributeIndex", selection.poolToSurface.at (face.materialPoolIndex));
        row.Add ("areas", DescribeAreas (face.areas));
        faces.Push (row);
    }
    for (const auto& fragment : result.fragments) {
        GS::ObjectState row;
        row.Add ("source", DescribeSource (snapshot, fragment.source));
        row.Add ("materialPoolIndex", fragment.materialPoolIndex);
        row.Add ("surfaceAttributeIndex", selection.poolToSurface.at (fragment.materialPoolIndex));
        row.Add ("selected", fragment.selected);
        row.Add ("areaM2", fragment.areaM2);
        row.Add ("points", Array (fragment.points));
        row.Add ("triangles", Array (fragment.triangles));
        GS::Array<GS::Array<double>> loops;
        for (const auto& loop : fragment.loops)
            loops.Push (Array (loop));
        row.Add ("loops", loops);
        fragments.Push (row);
    }
    std::map<int32_t, ex::Areas> bySurface;
    if (filter)
        for (int32_t surface : *filter)
            bySurface.emplace (surface, ex::Areas {});
    for (const auto& [pool, areas] : result.byMaterialPool) {
        auto& total = bySurface[selection.poolToSurface.at (pool)];
        total.grossGeometricArea += areas.grossGeometricArea;
        total.exposedArea += areas.exposedArea;
        total.selectedExposedArea += areas.selectedExposedArea;
    }
    for (const auto& [surface, areas] : bySurface) {
        GS::ObjectState row;
        row.Add ("surfaceAttributeIndex", surface);
        row.Add ("areas", DescribeAreas (areas));
        surfaces.Push (row);
    }
    for (const auto& [guid, areas] : result.byElementGuid) {
        GS::ObjectState row;
        row.Add ("elementId", ElementId (guid));
        row.Add ("areas", DescribeAreas (areas));
        elements.Push (row);
    }
    output.Add ("covers", covers);
    output.Add ("faces", faces);
    output.Add ("fragments", fragments);
    output.Add ("surfaces", surfaces);
    output.Add ("elements", elements);
    GS::ObjectState statistics;
    statistics.Add ("candidateCoverPairs", static_cast<GS::Int64> (result.statistics.candidateCoverPairs));
    statistics.Add ("sectionCount", static_cast<GS::Int64> (result.statistics.sectionCount));
    statistics.Add ("work", static_cast<GS::Int64> (result.statistics.work));
    statistics.Add ("outputTriangles", static_cast<GS::Int64> (result.statistics.outputTriangles));
    output.Add ("statistics", statistics);
    return output;
}

class GetSnapshotSurfacesCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetSnapshotSurfaces";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const auto snapshot = Pin (params);
        if (!snapshot || snapshot->surfaceAttributes.empty ())
            return NativeCommandResult::Failure (EVP_FAIL (
                "snapshot/catalog missing or stale; prepare a fresh snapshot", "Tapioca.GetSnapshotSurfaces"));
        GS::ObjectState output;
        output.Add ("snapshotId", static_cast<GS::Int64> (snapshot->id));
        output.Add ("captureStamp", static_cast<GS::Int64> (snapshot->captureStamp));
        output.Add ("scope", GS::UniString ("capturedProjectSurfaceCatalog"));
        GS::Array<GS::ObjectState> surfaces;
        for (const auto& [index, surface] : snapshot->surfaceAttributes) {
            GS::ObjectState row;
            row.Add ("surfaceAttributeIndex", index);
            row.Add ("name", Text (surface.name));
            row.Add ("guid", Text (surface.guid));
            surfaces.Push (row);
        }
        output.Add ("surfaces", surfaces);
        if (!IsCurrent (snapshot))
            return NativeCommandResult::Failure (
                EVP_FAIL ("snapshot changed during catalog read", "Tapioca.GetSnapshotSurfaces"));
        return output;
    }
};

class CalculateSurfaceFragmentsCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "CalculateSurfaceFragments";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl& processControl) const override
    {
        const auto fail = [] (const std::string& reason) {
            return NativeCommandResult::Failure (EVP_FAIL (Text (reason), "Tapioca.CalculateSurfaceFragments"));
        };
        const auto snapshot = Pin (params);
        if (!snapshot)
            return fail ("snapshot missing or stale; prepare a fresh snapshot");
        GS::Array<GS::ObjectState> faces, covers;
        GS::UniString scope;
        if (!params.Get ("faces", faces) || faces.IsEmpty () || faces.GetSize () > 512 ||
            !params.Get ("covers", covers) || covers.GetSize () > 128 || !params.Get ("coverScopeId", scope) ||
            scope.IsEmpty () || scope.GetLength () > 256)
            return fail ("explicit bounded faces, covers and coverScopeId are required");
        ex::Request request;
        request.snapshotId = snapshot->id;
        request.captureStamp = snapshot->captureStamp;
        request.coverScopeId = scope.ToCStr (0, MaxUSize, CC_UTF8).Get ();
        if (params.Contains ("contactToleranceM") && !ReadReal (params, "contactToleranceM", request.contactToleranceM))
            return fail ("invalid contactToleranceM");
        for (const auto& row : faces) {
            ex::FaceKey key;
            GS::Int32 polygon = 0;
            if (!ResolveBody (*snapshot, row, key.body) || !row.Get ("sourcePgon", polygon) || polygon <= 0)
                return fail ("target element/source face is missing, invalid or ambiguous");
            key.sourcePgon = polygon;
            request.faces.push_back (key);
        }
        for (const auto& row : covers) {
            ex::BodyKey key;
            if (!ResolveBody (*snapshot, row, key))
                return fail ("cover element/source body is missing, invalid or ambiguous");
            request.covers.push_back (key);
        }
        std::optional<std::set<int32_t>> surfaceFilter;
        if (params.Contains ("surfaceAttributeIndices")) {
            GS::Array<GS::Int32> indices;
            if (!params.Get ("surfaceAttributeIndices", indices) || indices.GetSize () > 512)
                return fail ("invalid surfaceAttributeIndices");
            surfaceFilter.emplace ();
            for (GS::Int32 index : indices)
                surfaceFilter->insert (index);
        }
        ex::SurfaceSelection selection;
        std::string error;
        if (!ex::ResolveSurfaceSelection (*snapshot, request.faces, surfaceFilter, selection, error))
            return fail (error);
        request.materialPoolFilter = selection.materialPoolFilter;
        ex::Result result;
        const auto cancelled = [&] { return processControl.TestBreak () || !IsCurrent (snapshot); };
        if (!ex::Calculate (*snapshot, request, result, error, {}, cancelled))
            return fail (error);
        auto output = DescribeResult (*snapshot, result, selection, surfaceFilter);
        // Guard AFTER serialization too. Every downstream consumer must still
        // compare the echoed capture identity before publishing an overlay.
        if (cancelled ())
            return fail ("snapshot changed or calculation cancelled before publication");
        return output;
    }
};

const NativeCommandRegistration kSurfaceFragmentRegistrations[] = {
    { "GetSnapshotSurfaces", &MakeRegisteredNativeCommand<GetSnapshotSurfacesCommand>, false, kCatalogInput,
      kCatalogOutput },
    { "CalculateSurfaceFragments", &MakeRegisteredNativeCommand<CalculateSurfaceFragmentsCommand>, false,
      kCalculateInput, kCalculateOutput }
};
} // namespace

NativeCommandRegistrations GetSurfaceFragmentCommandRegistrations ()
{
    return MakeRegistrationView (kSurfaceFragmentRegistrations);
}
} // namespace geomsrv
