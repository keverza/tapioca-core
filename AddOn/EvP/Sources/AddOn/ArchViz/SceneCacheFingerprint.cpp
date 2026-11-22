// ArchViz/SceneCacheFingerprint -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/SceneCacheFingerprint.hpp"

#include <ModelElement.hpp>
#include <ModelMeshBody.hpp>
#include <Polygon.hpp>
#include <AttributeIndex.hpp>
#include <Vertex.hpp>

#include <cctype>
#include <cmath>

namespace geomsrv {
namespace archviz {
namespace scenecache {

namespace {

// ⚠️ THE NULL GUID IS NOT AN ELEMENT (ModelContentWatch.hpp): elements with no GUID of their own
// all read as it, so a cache keyed by it would hand one element another's mesh.
constexpr const char* kNullGuid = "00000000-0000-0000-0000-000000000000";

std::string Utf8 (const GS::UniString& text)
{
    return text.ToCStr (0, MaxUSize, CC_UTF8).Get ();
}

} // namespace

CheapReading ReadCheap (const ModelerAPI::Element& element)
{
    CheapReading reading;
    const std::string guid = APIGuidToString (GSGuid2APIGuid (element.GetElemGuid ())).ToCStr ().Get ();
    if (guid != kNullGuid)
        reading.guid = guid;
    reading.genId = element.GetGenId ();
    const Int32 bodies = element.GetTessellatedBodyCount ();
    uint64_t hash = FnvValue (int32_t (bodies), kFnvOffset);
    for (Int32 b = 1; b <= bodies; ++b) {
        ModelerAPI::MeshBody body;
        element.GetTessellatedBody (b, &body);
        hash = FnvValue (int32_t (body.GetVertexCount ()), hash);
        hash = FnvValue (int32_t (body.GetPolygonCount ()), hash);
        hash = FnvValue (int32_t (body.GetEdgeCount ()), hash);
    }
    reading.shape = hash;
    return reading;
}

uint64_t ReadDeep (const ModelerAPI::Element& element, const std::map<int32_t, CapturedMaterialIdentity>& pool)
{
    uint64_t hash = kFnvOffset;
    const Int32 bodies = element.GetTessellatedBodyCount ();
    for (Int32 b = 1; b <= bodies; ++b) {
        ModelerAPI::MeshBody body;
        element.GetTessellatedBody (b, &body);
        const Int32 vertices = body.GetVertexCount ();
        const Int32 polygons = body.GetPolygonCount ();
        hash = FnvValue (int32_t (vertices), hash);
        hash = FnvValue (int32_t (polygons), hash);
        for (Int32 v = 1; v <= vertices; ++v) {
            ModelerAPI::Vertex vertex;
            body.GetVertex (v, &vertex); // world coordinates, as the extractor reads them
            const int64_t q[3] = { std::llround (vertex.x / kDeepQuantumM), std::llround (vertex.y / kDeepQuantumM),
                                   std::llround (vertex.z / kDeepQuantumM) };
            hash = Fnv (q, sizeof (q), hash);
        }
        for (Int32 p = 1; p <= polygons; ++p) {
            ModelerAPI::Polygon polygon;
            body.GetPolygon (p, &polygon);
            ModelerAPI::AttributeIndex surface;
            polygon.GetMaterialIndex (surface);
            const int32_t index = static_cast<int32_t> (surface.GetIndex ());
            // ⚠️ BY IDENTITY: the pool is renumbered between sessions, the surface is not.
            const auto found = pool.find (index);
            if (found != pool.end ()) {
                hash = FnvString (found->second.name, hash);
                hash = FnvValue (found->second.externalReference, hash);
            }
            else {
                hash = FnvValue (int32_t (-1), hash);
                hash = FnvValue (index, hash);
            }
        }
    }
    return hash == 0 ? 1 : hash; // 0 means "never read"
}

std::string ReadProjectIdentity ()
{
    API_ProjectInfo project;
    if (ACAPI_ProjectOperation_Project (&project) != NoError || project.untitled)
        return std::string ();
    if (project.teamwork) {
        if (project.projectPath == nullptr || project.projectName == nullptr)
            return std::string ();
        return "team:" + Utf8 (*project.projectPath) + "/" + Utf8 (*project.projectName);
    }
    GS::UniString path;
    if (project.location == nullptr || project.location->ToPath (&path) != NoError || path.IsEmpty ())
        return std::string ();
    // Windows paths are case-insensitive: one project, one file, however it was opened.
    std::string text = Utf8 (path);
    for (char& c : text)
        if (static_cast<unsigned char> (c) < 0x80)
            c = char (std::tolower (static_cast<unsigned char> (c)));
    return "solo:" + text;
}

} // namespace scenecache
} // namespace archviz
} // namespace geomsrv
