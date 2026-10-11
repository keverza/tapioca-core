#include "NativeCommands/SurfaceSourceCommands.hpp"
#include "NativeCommands/CommandBase.hpp"

#include "Geometry/MeshStore.hpp"
#include "Geometry/SurfaceSourceMap.hpp"

#include <algorithm>
#include <cctype>

namespace geomsrv {
namespace {

std::string CanonicalGuid (std::string guid)
{
    if (guid.size () == 38 && guid.front () == '{' && guid.back () == '}')
        guid = guid.substr (1, 36);
    std::transform (guid.begin (), guid.end (), guid.begin (),
                    [] (unsigned char c) { return static_cast<char> (std::toupper (c)); });
    return guid;
}

GS::Array<double> Vector3 (const double value[3])
{
    GS::Array<double> values;
    for (int i = 0; i < 3; ++i)
        values.Push (value[i]);
    return values;
}

GS::ObjectState DescribeFace (const surfacemap::SourceFace& face)
{
    GS::ObjectState row;
    row.Add ("sourceBody", face.sourceBody);
    row.Add ("sourcePgon", face.sourcePgon);
    row.Add ("areaM2", face.areaM2);
    row.Add ("centroid", Vector3 (face.centroid));
    row.Add ("normal", Vector3 (face.normal));
    GS::Array<GS::Int64> triangles;
    for (uint32_t triangle : face.triangles)
        triangles.Push (static_cast<GS::Int64> (triangle));
    row.Add ("sourceTriangles", triangles);
    return row;
}

// Read only: the caller pins the snapshot and capture epoch. No SDK calls and
// no implied equality between C-database/picker and ModelerAPI index spaces.
class InspectSurfaceSourceFaceCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "InspectSurfaceSourceFace";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState elementId;
        GS::UniString guid;
        GS::Int64 snapshotId = 0, captureStamp = 0;
        if (!params.Get ("elementId", elementId) || !elementId.Get ("guid", guid) || guid.IsEmpty () ||
            !params.Get ("snapshotId", snapshotId) || snapshotId <= 0 || !params.Get ("captureStamp", captureStamp) ||
            captureStamp <= 0)
            return NativeCommandResult::Failure (EVP_FAIL (
                "elementId, snapshotId and nonzero captureStamp are required", "Tapioca.InspectSurfaceSourceFace"));

        const auto snapshot = MeshStore::Get ().Current ();
        if (!snapshot || snapshot->id != static_cast<uint64_t> (snapshotId) ||
            snapshot->captureStamp != static_cast<uint64_t> (captureStamp) ||
            MeshStore::Get ().CaptureStamp () != static_cast<uint64_t> (captureStamp))
            return NativeCommandResult::Failure (
                EVP_FAIL ("snapshot missing or stale; build a fresh snapshot", "Tapioca.InspectSurfaceSourceFace"));

        const std::string key = CanonicalGuid (guid.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        const Mesh* mesh = nullptr;
        for (const Mesh& candidate : snapshot->meshes) {
            if (CanonicalGuid (candidate.guid) != key)
                continue;
            if (mesh != nullptr)
                return NativeCommandResult::Failure (
                    EVP_FAIL ("multiple meshes have this element identity", "Tapioca.InspectSurfaceSourceFace"));
            mesh = &candidate;
        }
        if (mesh == nullptr)
            return NativeCommandResult::Failure (
                EVP_FAIL ("element is absent from the snapshot", "Tapioca.InspectSurfaceSourceFace"));

        std::string reason;
        const auto faces = surfacemap::EnumerateSourceFaces (*mesh, &reason);
        GS::ObjectState output;
        output.Add ("snapshotId", snapshotId);
        output.Add ("captureStamp", captureStamp);
        output.Add ("elementId", elementId);
        output.Add ("unit", GS::UniString ("m2"));
        output.Add ("indexSpace", GS::UniString ("ModelerAPI-1-based"));
        GS::Array<GS::ObjectState> rows;
        for (const auto& face : faces)
            rows.Push (DescribeFace (face));
        output.Add ("faces", rows);

        GS::Array<double> polygonPoints;
        GS::Array<GS::Array<double>> polygonPieces;
        const bool havePoints = params.Get ("polygonPoints", polygonPoints);
        const bool havePieces = params.Get ("polygonPieces", polygonPieces);
        const bool haveQuery = havePoints || havePieces;
        const char* status = reason.empty () ? "enumerated" : "invalid";
        if (reason.empty () && haveQuery) {
            surfacemap::FaceQuery query;
            query.points.assign (polygonPoints.Begin (), polygonPoints.End ());
            for (const auto& piece : polygonPieces)
                query.pieces.emplace_back (piece.Begin (), piece.End ());
            const auto match = surfacemap::Resolve (faces, query);
            reason = match.reason;
            switch (match.status) {
                case surfacemap::MatchStatus::Ok:
                    status = "matched";
                    output.Add ("matchedFace", DescribeFace (match.face));
                    break;
                case surfacemap::MatchStatus::Invalid:
                    status = "invalid";
                    break;
                case surfacemap::MatchStatus::NoMatch:
                    status = "noMatch";
                    break;
                case surfacemap::MatchStatus::Ambiguous:
                    status = "ambiguous";
                    break;
            }
        }
        if (MeshStore::Get ().Current () != snapshot ||
            MeshStore::Get ().CaptureStamp () != static_cast<uint64_t> (captureStamp))
            return NativeCommandResult::Failure (
                EVP_FAIL ("snapshot changed during inspection", "Tapioca.InspectSurfaceSourceFace"));
        output.Add ("status", GS::UniString (status));
        output.Add ("reason", GS::UniString (reason.c_str (), CC_UTF8));
        return output;
    }
};

constexpr const char kSourceInput[] =
    R"json({
    "type":"object",
    "properties":{
        "elementId":{"$ref":"#/$defs/elementId"},
        "snapshotId":{"type":"integer","minimum":1},
        "captureStamp":{"type":"integer","minimum":1},
        "polygonPoints":{
            "$ref":"#/$defs/contour",
            "description":"Outer planar contour in world-space metres. Omit both geometry fields to enumerate only."
        },
        "polygonPieces":{
            "type":"array","minItems":1,"items":{"$ref":"#/$defs/contour"},
            "description":"Additional opposite-wound holes, or same-wound convex decomposition parts. May be used without polygonPoints."
        }
    },
    "additionalProperties":false,
    "required":["elementId","snapshotId","captureStamp"],
    "$defs":{
        "elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},
        "contour":{"type":"array","minItems":9,"items":{"type":"number"}}
    }
})json";
constexpr const char kSourceOutput[] =
    R"json({"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"snapshotId":{"type":"integer","minimum":1},"captureStamp":{"type":"integer","minimum":1},"unit":{"const":"m2"},"indexSpace":{"const":"ModelerAPI-1-based"},"status":{"enum":["enumerated","matched","invalid","noMatch","ambiguous"]},"reason":{"type":"string"},"faces":{"type":"array","items":{"$ref":"#/$defs/face"}},"matchedFace":{"$ref":"#/$defs/face"}},"additionalProperties":false,"required":["elementId","snapshotId","captureStamp","unit","indexSpace","status","reason","faces"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"vector3":{"type":"array","minItems":3,"maxItems":3,"items":{"type":"number"}},"face":{"type":"object","properties":{"sourceBody":{"type":"integer","minimum":1},"sourcePgon":{"type":"integer","minimum":1},"areaM2":{"type":"number","minimum":0},"centroid":{"$ref":"#/$defs/vector3"},"normal":{"$ref":"#/$defs/vector3"},"sourceTriangles":{"type":"array","items":{"type":"integer","minimum":0}}},"additionalProperties":false,"required":["sourceBody","sourcePgon","areaM2","centroid","normal","sourceTriangles"]}}})json";

const NativeCommandRegistration kSurfaceSourceCommandRegistrations[] = {
    { "InspectSurfaceSourceFace", &MakeRegisteredNativeCommand<InspectSurfaceSourceFaceCommand>, false, kSourceInput,
      kSourceOutput },
};

} // namespace

NativeCommandRegistrations GetSurfaceSourceCommandRegistrations ()
{
    return MakeRegistrationView (kSurfaceSourceCommandRegistrations);
}

} // namespace geomsrv
