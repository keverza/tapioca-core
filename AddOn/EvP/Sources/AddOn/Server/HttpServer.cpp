#include "HttpServer.hpp"
#include "ServerState.hpp"
#include "AddOnVersion.hpp" // ADDON_VERSION — pasted into the /health literals
#include "Geometry/MeshStore.hpp"
#include "Geometry/MeshSerializer.hpp"
#include "Geometry/SpatialQueries.hpp"
#include "Geometry/QueryEngine.hpp"
#include "Geometry/ClashEngine.hpp"
#include "Geometry/RenderEngine.hpp"
#include "Geometry/SliceEngine.hpp"
#include "Screenshot/ScreenshotStore.hpp"
#include "Metadata/MetadataStore.hpp"
#include "Metadata/MetadataJson.hpp"
#include "Python/ApiDispatcher.hpp" // Zone C: /evp/call routes to the same bus

#include <httplib.h>
#include <string>
#include <memory>
#include <vector>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace geomsrv {

namespace {

// Parse a comma-separated list of doubles ("1.0,2,3.5").
std::vector<double> ParseDoubles (const std::string& s)
{
    std::vector<double> out;
    size_t i = 0;
    while (i < s.size ()) {
        size_t j = s.find (',', i);
        if (j == std::string::npos)
            j = s.size ();
        if (j > i)
            out.push_back (std::atof (s.substr (i, j - i).c_str ()));
        i = j + 1;
    }
    return out;
}

// JSON array of GUID strings (GUIDs are hex+dashes — no escaping needed).
std::string GuidsToJson (uint64_t snapshotId, const std::string& scope, const std::vector<std::string>& guids)
{
    std::string body = "{\"ok\":true,\"snapshotId\":" + std::to_string (snapshotId) + ",\"scope\":\"" + scope +
                       "\",\"count\":" + std::to_string (guids.size ()) + ",\"guids\":[";
    for (size_t i = 0; i < guids.size (); ++i) {
        if (i)
            body += ',';
        body += '"';
        body += guids[i];
        body += '"';
    }
    body += "]}";
    return body;
}

// Compact double for JSON (enough precision for mm, no trailing garbage).
std::string Num (double v)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%.6g", v);
    return buf;
}
std::string Vec3Json (const double v[3])
{
    return "[" + Num (v[0]) + "," + Num (v[1]) + "," + Num (v[2]) + "]";
}

// Little-endian POD append/read for the binary batch protocol.
template <class T> void PutLE (std::string& b, T v)
{
    b.append (reinterpret_cast<const char*> (&v), sizeof (T));
}
template <class T> bool GetLE (const std::string& b, size_t& off, T& v)
{
    if (off + sizeof (T) > b.size ())
        return false;
    std::memcpy (&v, b.data () + off, sizeof (T));
    off += sizeof (T);
    return true;
}

// Fetch the current snapshot, or write a 409 and return null. Honours an
// optional ?snapshot=<id> consistency check.
std::shared_ptr<const Snapshot> RequireSnapshot (const httplib::Request& req, httplib::Response& res)
{
    auto snap = MeshStore::Get ().Current ();
    if (!snap) {
        res.status = 409;
        res.set_content ("{\"ok\":false,\"error\":\"no snapshot; press Send All/Selection\"}", "application/json");
        return nullptr;
    }
    if (req.has_param ("snapshot")) {
        const uint64_t want = std::strtoull (req.get_param_value ("snapshot").c_str (), nullptr, 10);
        if (want != snap->id) {
            res.status = 409;
            res.set_content ("{\"ok\":false,\"error\":\"stale snapshot\",\"snapshotId\":" + std::to_string (snap->id) +
                                 "}",
                             "application/json");
            return nullptr;
        }
    }
    return snap;
}

// Fetch (building/caching if needed) the query engine for the current snapshot,
// or write a 409 and return null.
std::shared_ptr<const QueryEngine> RequireEngine (const httplib::Request& req, httplib::Response& res)
{
    auto snap = RequireSnapshot (req, res);
    if (!snap)
        return nullptr;
    return QueryIndexCache::Get ().For (snap);
}

} // namespace

HttpServer::HttpServer () : server (std::make_unique<httplib::Server> ())
{
}

HttpServer::~HttpServer ()
{
    Stop ();
}

void HttpServer::SetWebUIPage (const std::string& html)
{
    std::lock_guard<std::mutex> lock (webUiMutex);
    webUiPage = html;
}

void HttpServer::RegisterRoutes ()
{
    // Liveness. Fully ACAPI-free — reads only the atomics in ServerState. Never
    // touches Archicad's main thread, so it can never block it.
    //
    // NOTE: this WAS a pure data plane — main-thread work lived on Archicad's own
    // JSON command channel, because that was the only channel Archicad dispatched
    // while its Python palette blocked the event loop. That constraint is GONE: EvP
    // hosts the interpreter on its own worker now, so the main loop stays free and
    // P1 proved CallFromEventLoop dispatches. /evp/call below therefore marshals to
    // the main thread from here, which would have deadlocked under the old design.
    server->Get ("/health", [] (const httplib::Request&, httplib::Response& res) {
        ServerState& st = ServerState::Get ();
        const std::string body = std::string ("{\"ok\":true,") + "\"addon\":\"EvP\"," +
                                 "\"version\":\"" ADDON_VERSION "\"," +
                                 "\"acVersion\":" + std::to_string (AC_VERSION_NUM) + "," +
                                 "\"modelOpen\":" + (st.modelOpen.load () ? "true" : "false") + "," +
                                 "\"snapshotId\":" + std::to_string (st.snapshotId.load ()) + "}";
        res.set_content (body, "application/json");
    });

    // The embedded browser starts on this same-origin route. The page is supplied
    // by the native palette from a validated DATA resource; no writable directory
    // or arbitrary file serving is involved.
    const auto serveWebUI = [this] (const httplib::Request&, httplib::Response& res) {
        std::string page;
        {
            std::lock_guard<std::mutex> lock (webUiMutex);
            page = webUiPage;
        }
        if (page.empty ()) {
            res.status = 503;
            res.set_content ("{\"ok\":false,\"error\":\"WebUI page is not registered\"}", "application/json");
            return;
        }
        res.set_content (page, "text/html; charset=UTF-8");
    };
    server->Get ("/ui", serveWebUI);
    server->Get ("/ui/", serveWebUI);
    server->Get ("/ui/index.html", serveWebUI);

    // ---- Zone C: the external-runtime bus ---------------------------------
    // The ONE endpoint a `runtime="external"` subprocess talks to. It is the exact
    // same envelope the in-process `_evp.call` produces, routed into the exact same
    // dispatcher — so a script cannot tell which zone it runs in, which is the
    // design requirement, not a nicety. Only `meta.zone` differs.
    //
    // This call marshals to Archicad's main thread and BLOCKS this httplib worker
    // until it returns (~0.6-8 ms, or as long as a user takes to answer a pick).
    // That is correct: the caller is a subprocess waiting on the reply anyway.
    server->Post ("/evp/call", [] (const httplib::Request& req, httplib::Response& res) {
        std::string command = req.get_param_value ("command");
        if (command.empty ()) {
            // Never guess a command name: a typo'd route must fail loudly, not run
            // something adjacent.
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":{\"code\":\"BadRequest\","
                             "\"message\":\"missing ?command=\"}}",
                             "application/json");
            return;
        }
        const GS::UniString envelope =
            evp::DispatchApiCall (GS::UniString (command.c_str (), CC_UTF8),
                                  GS::UniString (req.body.empty () ? "{}" : req.body.c_str (), CC_UTF8), "external");
        res.set_content (envelope.ToCStr (0, MaxUSize, CC_UTF8).Get (), "application/json");
    });

    // Zone C handshake. The subprocess checks this BEFORE running a script: a major
    // mismatch means the bundled `evp` package and this add-on disagree about the
    // API, and failing here beats failing halfway through a transaction.
    server->Get ("/evp/version", [] (const httplib::Request&, httplib::Response& res) {
        res.set_content (std::string ("{\"ok\":true,\"api_version\":\"") + evp::ApiVersion +
                             "\",\"addon\":\"EvP\",\"version\":\"" ADDON_VERSION "\"}",
                         "application/json");
    });

    // Snapshot summary (JSON). Reads the cached snapshot; never calls ACAPI.
    server->Get ("/snapshot", [] (const httplib::Request&, httplib::Response& res) {
        auto snap = MeshStore::Get ().Current ();
        std::string body;
        if (!snap) {
            body = "{\"ok\":true,\"built\":false}";
        }
        else {
            const size_t bytes =
                MeshStore::Get ().Bytes () + MetadataStore::Get ().Bytes () + ScreenshotStore::Get ().Bytes ();
            body = std::string ("{\"ok\":true,\"built\":true,") + "\"snapshotId\":" + std::to_string (snap->id) + "," +
                   "\"scope\":\"" + snap->scope + "\"," + "\"elementCount\":" + std::to_string (snap->meshes.size ()) +
                   "," + "\"vertexCount\":" + std::to_string (snap->TotalVertices ()) + "," +
                   "\"triangleCount\":" + std::to_string (snap->TotalTriangles ()) + "," +
                   "\"hasMetadata\":" + (MetadataStore::Get ().Current () ? "true" : "false") + "," +
                   "\"retainedBytes\":" + std::to_string (bytes) + "}";
        }
        res.set_content (body, "application/json");
    });

    // All meshes in the current snapshot (msgpack). Empty 409 if not built yet.
    server->Get ("/meshes", [] (const httplib::Request&, httplib::Response& res) {
        auto snap = MeshStore::Get ().Current ();
        if (!snap) {
            res.status = 409;
            res.set_content ("{\"ok\":false,\"error\":\"no snapshot; press Build\"}", "application/json");
            return;
        }
        res.set_content (SerializeSnapshot (*snap), "application/x-msgpack");
    });

    // One mesh by GUID (msgpack). 404 if absent, 409 if no snapshot.
    server->Get ("/mesh", [] (const httplib::Request& req, httplib::Response& res) {
        auto snap = MeshStore::Get ().Current ();
        if (!snap) {
            res.status = 409;
            res.set_content ("{\"ok\":false,\"error\":\"no snapshot; press Build\"}", "application/json");
            return;
        }
        const std::string guid = req.has_param ("guid") ? req.get_param_value ("guid") : "";
        for (const auto& m : snap->meshes) {
            if (m.guid == guid) {
                res.set_content (SerializeMesh (m), "application/x-msgpack");
                return;
            }
        }
        res.status = 404;
        res.set_content ("{\"ok\":false,\"error\":\"guid not found\"}", "application/json");
    });

    // Broadphase queries against element AABBs -> candidate GUIDs (JSON).
    // /query/box?min=x,y,z&max=x,y,z
    server->Get ("/query/box", [] (const httplib::Request& req, httplib::Response& res) {
        auto snap = RequireSnapshot (req, res);
        if (!snap)
            return;
        const auto mn = ParseDoubles (req.get_param_value ("min"));
        const auto mx = ParseDoubles (req.get_param_value ("max"));
        if (mn.size () != 3 || mx.size () != 3) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"need min=x,y,z & max=x,y,z\"}", "application/json");
            return;
        }
        const double bmn[3] = { mn[0], mn[1], mn[2] };
        const double bmx[3] = { mx[0], mx[1], mx[2] };
        res.set_content (GuidsToJson (snap->id, snap->scope, QueryBox (*snap, bmn, bmx)), "application/json");
    });

    // /query/sphere?c=x,y,z&r=R
    server->Get ("/query/sphere", [] (const httplib::Request& req, httplib::Response& res) {
        auto snap = RequireSnapshot (req, res);
        if (!snap)
            return;
        const auto c = ParseDoubles (req.get_param_value ("c"));
        const double r = std::atof (req.get_param_value ("r").c_str ());
        if (c.size () != 3 || r <= 0.0) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"need c=x,y,z & r>0\"}", "application/json");
            return;
        }
        const double cc[3] = { c[0], c[1], c[2] };
        res.set_content (GuidsToJson (snap->id, snap->scope, QuerySphere (*snap, cc, r)), "application/json");
    });

    // /query/polygon?pts=x0,y0,x1,y1,...&zmin=..&zmax=..
    server->Get ("/query/polygon", [] (const httplib::Request& req, httplib::Response& res) {
        auto snap = RequireSnapshot (req, res);
        if (!snap)
            return;
        const auto pts = ParseDoubles (req.get_param_value ("pts"));
        if (pts.size () < 6 || pts.size () % 2 != 0) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"need pts=x0,y0,x1,y1,... (>=3 pts)\"}", "application/json");
            return;
        }
        const double zmin = req.has_param ("zmin") ? std::atof (req.get_param_value ("zmin").c_str ()) : -1e30;
        const double zmax = req.has_param ("zmax") ? std::atof (req.get_param_value ("zmax").c_str ()) : 1e30;
        res.set_content (GuidsToJson (snap->id, snap->scope, QueryPolygon (*snap, pts, zmin, zmax)),
                         "application/json");
    });

    // ---- M5: narrowphase queries over the triangle BVH --------------------

    // GUID index table in snapshot->meshes order, so batch callers can map the
    // integer element index in binary responses back to a GUID. (JSON)
    server->Get ("/guids", [] (const httplib::Request& req, httplib::Response& res) {
        auto snap = RequireSnapshot (req, res);
        if (!snap)
            return;
        std::vector<std::string> guids;
        guids.reserve (snap->meshes.size ());
        for (const auto& m : snap->meshes)
            guids.push_back (m.guid);
        res.set_content (GuidsToJson (snap->id, snap->scope, guids), "application/json");
    });

    // Single raycast: /ray?o=x,y,z&d=x,y,z&max=1e30   (JSON)
    server->Get ("/ray", [] (const httplib::Request& req, httplib::Response& res) {
        auto eng = RequireEngine (req, res);
        if (!eng)
            return;
        const auto o = ParseDoubles (req.get_param_value ("o"));
        const auto d = ParseDoubles (req.get_param_value ("d"));
        if (o.size () != 3 || d.size () != 3) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"need o=x,y,z & d=x,y,z\"}", "application/json");
            return;
        }
        const double maxD = req.has_param ("max") ? std::atof (req.get_param_value ("max").c_str ()) : 0.0;
        const double org[3] = { o[0], o[1], o[2] };
        const double dir[3] = { d[0], d[1], d[2] };
        const auto h = eng->Raycast (org, dir, maxD);
        std::string body = "{\"ok\":true,\"hit\":";
        body += h.hit ? "true" : "false";
        if (h.hit) {
            body += ",\"guid\":\"" + eng->MeshGuid (h.meshIndex) + "\"";
            body += ",\"type\":" + std::to_string (eng->MeshType (h.meshIndex));
            body += ",\"t\":" + Num (h.t);
            body += ",\"point\":" + Vec3Json (h.point);
            body += ",\"normal\":" + Vec3Json (h.normal);
        }
        body += "}";
        res.set_content (body, "application/json");
    });

    // Closest surface point: /closest?p=x,y,z&max=..   (JSON)
    server->Get ("/closest", [] (const httplib::Request& req, httplib::Response& res) {
        auto eng = RequireEngine (req, res);
        if (!eng)
            return;
        const auto p = ParseDoubles (req.get_param_value ("p"));
        if (p.size () != 3) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"need p=x,y,z\"}", "application/json");
            return;
        }
        const double maxD = req.has_param ("max") ? std::atof (req.get_param_value ("max").c_str ()) : 0.0;
        const double pt[3] = { p[0], p[1], p[2] };
        const auto h = eng->ClosestPoint (pt, maxD);
        std::string body = "{\"ok\":true,\"found\":";
        body += h.found ? "true" : "false";
        if (h.found) {
            body += ",\"guid\":\"" + eng->MeshGuid (h.meshIndex) + "\"";
            body += ",\"type\":" + std::to_string (eng->MeshType (h.meshIndex));
            body += ",\"dist\":" + Num (h.dist);
            body += ",\"point\":" + Vec3Json (h.point);
        }
        body += "}";
        res.set_content (body, "application/json");
    });

    // Nearest elements by AABB distance: /nearest?p=x,y,z&k=5   (JSON)
    server->Get ("/nearest", [] (const httplib::Request& req, httplib::Response& res) {
        auto eng = RequireEngine (req, res);
        if (!eng)
            return;
        const auto p = ParseDoubles (req.get_param_value ("p"));
        if (p.size () != 3) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"need p=x,y,z\"}", "application/json");
            return;
        }
        const size_t k = req.has_param ("k")
                             ? static_cast<size_t> (std::strtoul (req.get_param_value ("k").c_str (), nullptr, 10))
                             : 5;
        const double pt[3] = { p[0], p[1], p[2] };
        const auto ns = eng->NearestElement (pt, k);
        std::string body = "{\"ok\":true,\"count\":" + std::to_string (ns.size ()) + ",\"neighbours\":[";
        for (size_t i = 0; i < ns.size (); ++i) {
            if (i)
                body += ',';
            body += "{\"guid\":\"" + eng->MeshGuid (ns[i].meshIndex) +
                    "\",\"type\":" + std::to_string (eng->MeshType (ns[i].meshIndex)) +
                    ",\"dist\":" + Num (ns[i].dist) + "}";
        }
        body += "]}";
        res.set_content (body, "application/json");
    });

    // Batch raycast (binary, for 10^4+ rays without per-request overhead).
    //   Request  body: int32 N, float64 maxDist, float64[N*3] origins, float64[N*3] dirs
    //   Response body: int32 N, uint8[N] hit, int32[N] elem (-1 = miss),
    //                  float64[N] t, float64[N*3] point, float64[N*3] normal
    // `elem` indexes snapshot->meshes (see /guids). All little-endian.
    server->Post ("/ray/batch", [] (const httplib::Request& req, httplib::Response& res) {
        auto eng = RequireEngine (req, res);
        if (!eng)
            return;
        size_t off = 0;
        int32_t n = 0;
        double maxD = 0.0;
        if (!GetLE (req.body, off, n) || !GetLE (req.body, off, maxD) || n < 0 ||
            req.body.size () < off + static_cast<size_t> (n) * 6 * sizeof (double)) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"bad batch body\"}", "application/json");
            return;
        }
        const double* org = reinterpret_cast<const double*> (req.body.data () + off);
        const double* dir = org + static_cast<size_t> (n) * 3;

        std::string hitB, elemB, tB, pB, nB;
        hitB.reserve (n);
        elemB.reserve (n * 4);
        tB.reserve (n * 8);
        pB.reserve (n * 24);
        nB.reserve (n * 24);
        for (int32_t i = 0; i < n; ++i) {
            const auto h = eng->Raycast (&org[i * 3], &dir[i * 3], maxD);
            PutLE<uint8_t> (hitB, h.hit ? 1 : 0);
            PutLE<int32_t> (elemB, h.hit ? static_cast<int32_t> (h.meshIndex) : -1);
            PutLE<double> (tB, h.hit ? h.t : 0.0);
            for (int k = 0; k < 3; ++k)
                PutLE<double> (pB, h.point[k]);
            for (int k = 0; k < 3; ++k)
                PutLE<double> (nB, h.normal[k]);
        }
        std::string body;
        PutLE<int32_t> (body, n);
        body += hitB;
        body += elemB;
        body += tB;
        body += pB;
        body += nB;
        res.set_content (body, "application/octet-stream");
    });

    // Batch closest point (binary).
    //   Request  body: int32 N, float64 maxDist, float64[N*3] points
    //   Response body: int32 N, uint8[N] found, int32[N] elem, float64[N] dist,
    //                  float64[N*3] point
    server->Post ("/closest/batch", [] (const httplib::Request& req, httplib::Response& res) {
        auto eng = RequireEngine (req, res);
        if (!eng)
            return;
        size_t off = 0;
        int32_t n = 0;
        double maxD = 0.0;
        if (!GetLE (req.body, off, n) || !GetLE (req.body, off, maxD) || n < 0 ||
            req.body.size () < off + static_cast<size_t> (n) * 3 * sizeof (double)) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"bad batch body\"}", "application/json");
            return;
        }
        const double* pts = reinterpret_cast<const double*> (req.body.data () + off);
        std::string foundB, elemB, dB, pB;
        foundB.reserve (n);
        elemB.reserve (n * 4);
        dB.reserve (n * 8);
        pB.reserve (n * 24);
        for (int32_t i = 0; i < n; ++i) {
            const auto h = eng->ClosestPoint (&pts[i * 3], maxD);
            PutLE<uint8_t> (foundB, h.found ? 1 : 0);
            PutLE<int32_t> (elemB, h.found ? static_cast<int32_t> (h.meshIndex) : -1);
            PutLE<double> (dB, h.found ? h.dist : 0.0);
            for (int k = 0; k < 3; ++k)
                PutLE<double> (pB, h.point[k]);
        }
        std::string body;
        PutLE<int32_t> (body, n);
        body += foundB;
        body += elemB;
        body += dB;
        body += pB;
        res.set_content (body, "application/octet-stream");
    });

    // ---- M6: clash (hard intersection) & clearance (min distance) ---------

    // Hard clash between two elements: /clash?a=<guid>&b=<guid>   (JSON)
    server->Get ("/clash", [] (const httplib::Request& req, httplib::Response& res) {
        auto snap = RequireSnapshot (req, res);
        if (!snap)
            return;
        const Mesh* a = snap->FindMesh (req.get_param_value ("a"));
        const Mesh* b = snap->FindMesh (req.get_param_value ("b"));
        if (!a || !b) {
            res.status = 404;
            res.set_content ("{\"ok\":false,\"error\":\"guid a and/or b not found\"}", "application/json");
            return;
        }
        const bool hit = MeshesClash (*a, *b);
        res.set_content (std::string ("{\"ok\":true,\"clash\":") + (hit ? "true" : "false") + "}", "application/json");
    });

    // Min surface-to-surface distance: /clearance?a=<guid>&b=<guid>&max=..  (JSON)
    server->Get ("/clearance", [] (const httplib::Request& req, httplib::Response& res) {
        auto snap = RequireSnapshot (req, res);
        if (!snap)
            return;
        const Mesh* a = snap->FindMesh (req.get_param_value ("a"));
        const Mesh* b = snap->FindMesh (req.get_param_value ("b"));
        if (!a || !b) {
            res.status = 404;
            res.set_content ("{\"ok\":false,\"error\":\"guid a and/or b not found\"}", "application/json");
            return;
        }
        const double maxD = req.has_param ("max") ? std::atof (req.get_param_value ("max").c_str ()) : 0.0;
        const Clearance c = MeshClearance (*a, *b, maxD);
        std::string body = "{\"ok\":true,\"clash\":";
        body += c.clash ? "true" : "false";
        if (c.dist >= 0.0) {
            body += ",\"dist\":" + Num (c.dist);
            body += ",\"pointA\":" + Vec3Json (c.pointA);
            body += ",\"pointB\":" + Vec3Json (c.pointB);
        }
        body += "}";
        res.set_content (body, "application/json");
    });

    // All clashing/near pairs in the snapshot: /clash/all?gap=<d>   (JSON)
    // gap omitted or <=0 -> only true intersections; gap>0 -> pairs within gap.
    server->Get ("/clash/all", [] (const httplib::Request& req, httplib::Response& res) {
        auto snap = RequireSnapshot (req, res);
        if (!snap)
            return;
        const double gap = req.has_param ("gap") ? std::atof (req.get_param_value ("gap").c_str ()) : 0.0;
        const auto pairs = ClashAll (*snap, gap);
        std::string body = "{\"ok\":true,\"snapshotId\":" + std::to_string (snap->id) + ",\"gap\":" + Num (gap) +
                           ",\"count\":" + std::to_string (pairs.size ()) + ",\"pairs\":[";
        for (size_t i = 0; i < pairs.size (); ++i) {
            if (i)
                body += ',';
            const auto& p = pairs[i];
            body += "{\"a\":\"" + snap->meshes[p.i].guid + "\",\"b\":\"" + snap->meshes[p.j].guid +
                    "\",\"typeA\":" + std::to_string (snap->meshes[p.i].elemType) +
                    ",\"typeB\":" + std::to_string (snap->meshes[p.j].elemType) + ",\"dist\":" + Num (p.dist) + "}";
        }
        body += "]}";
        res.set_content (body, "application/json");
    });

    // ---- M7: CPU render buffers (depth / normal / object-id) --------------
    // GET /render?eye=x,y,z&target=x,y,z[&up=x,y,z][&fov=50][&w=512][&h=512]
    //            [&proj=persp|ortho][&orthoH=<m>]
    // Response (binary, little-endian):
    //   int32 w, int32 h,
    //   int32[w*h]   elem   (-1 = background; map via /guids),
    //   float32[w*h] depth  (ray distance in meters; 0 = background),
    //   float32[w*h*3] normal (unit smooth normal; 0 = background)
    // Rows top-to-bottom; pixel (r,c) at index r*w + c.
    server->Get ("/render", [] (const httplib::Request& req, httplib::Response& res) {
        auto eng = RequireEngine (req, res);
        if (!eng)
            return;
        const auto eye = ParseDoubles (req.get_param_value ("eye"));
        const auto tgt = ParseDoubles (req.get_param_value ("target"));
        if (eye.size () != 3 || tgt.size () != 3) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"need eye=x,y,z & target=x,y,z\"}", "application/json");
            return;
        }
        Camera cam;
        cam.eye[0] = eye[0];
        cam.eye[1] = eye[1];
        cam.eye[2] = eye[2];
        cam.target[0] = tgt[0];
        cam.target[1] = tgt[1];
        cam.target[2] = tgt[2];
        if (req.has_param ("up")) {
            const auto up = ParseDoubles (req.get_param_value ("up"));
            if (up.size () == 3) {
                cam.up[0] = up[0];
                cam.up[1] = up[1];
                cam.up[2] = up[2];
            }
        }
        if (req.has_param ("fov"))
            cam.fovYdeg = std::atof (req.get_param_value ("fov").c_str ());
        if (req.has_param ("orthoH"))
            cam.orthoHeight = std::atof (req.get_param_value ("orthoH").c_str ());
        cam.ortho = req.has_param ("proj") && req.get_param_value ("proj") == "ortho";

        int w = req.has_param ("w") ? std::atoi (req.get_param_value ("w").c_str ()) : 512;
        int h = req.has_param ("h") ? std::atoi (req.get_param_value ("h").c_str ()) : 512;
        w = std::max (1, std::min (w, 4096)); // guard rails
        h = std::max (1, std::min (h, 4096));

        const RenderResult img = Render (*eng, cam, w, h);
        std::string body;
        body.reserve (8 + img.id.size () * 4 + img.depth.size () * 4 + img.normal.size () * 4);
        PutLE<int32_t> (body, img.w);
        PutLE<int32_t> (body, img.h);
        body.append (reinterpret_cast<const char*> (img.id.data ()), img.id.size () * 4);
        body.append (reinterpret_cast<const char*> (img.depth.data ()), img.depth.size () * 4);
        body.append (reinterpret_cast<const char*> (img.normal.data ()), img.normal.size () * 4);
        res.set_content (body, "application/octet-stream");
    });

    // Native 3D screenshots (PNG), captured on the main thread via the palette
    // "Screenshots" button. /screenshot/current = live view, /screenshot/top =
    // top-down. 409 until the button has been pressed at least once.
    auto serveShot = [] (std::shared_ptr<const Shot> s, httplib::Response& res) {
        if (!s || s->png.empty ()) {
            res.status = 409;
            res.set_content (
                "{\"ok\":false,\"error\":\"no screenshot; press Screenshots in the panel (in a 3D window)\"}",
                "application/json");
            return;
        }
        res.set_content (s->png, "image/png");
    };
    server->Get ("/screenshot/current", [serveShot] (const httplib::Request&, httplib::Response& res) {
        serveShot (ScreenshotStore::Get ().Current (), res);
    });
    server->Get ("/screenshot/top", [serveShot] (const httplib::Request&, httplib::Response& res) {
        serveShot (ScreenshotStore::Get ().Top (), res);
    });

    // ---- M8: element metadata (cached at Send All) ------------------------

    // One element's metadata: /meta?guid=<GUID>   (JSON)
    server->Get ("/meta", [] (const httplib::Request& req, httplib::Response& res) {
        auto set = MetadataStore::Get ().Current ();
        if (!set) {
            res.status = 409;
            res.set_content ("{\"ok\":false,\"error\":\"no metadata; press Send All\"}", "application/json");
            return;
        }
        const ElementMeta* m = set->Find (req.get_param_value ("guid"));
        if (!m) {
            res.status = 404;
            res.set_content ("{\"ok\":false,\"error\":\"guid not found\"}", "application/json");
            return;
        }
        std::string body = "{\"ok\":true,\"meta\":";
        ElementMetaToJson (*m, body);
        body += "}";
        res.set_content (body, "application/json");
    });

    // All elements' metadata: /metas   (JSON array)
    server->Get ("/metas", [] (const httplib::Request&, httplib::Response& res) {
        auto set = MetadataStore::Get ().Current ();
        if (!set) {
            res.status = 409;
            res.set_content ("{\"ok\":false,\"error\":\"no metadata; press Send All\"}", "application/json");
            return;
        }
        std::string body = "{\"ok\":true,\"count\":" + std::to_string (set->elems.size ()) + ",\"metas\":[";
        for (size_t i = 0; i < set->elems.size (); ++i) {
            if (i)
                body += ',';
            ElementMetaToJson (set->elems[i], body);
        }
        body += "]}";
        res.set_content (body, "application/json");
    });

    // Project stories with numeric elevations: /stories   (JSON)
    // `elevation` is world Z (same frame as geometry). `height` is the gap to the
    // story above; the topmost story reports null (it has no story above it).
    server->Get ("/stories", [] (const httplib::Request&, httplib::Response& res) {
        auto set = MetadataStore::Get ().Current ();
        if (!set) {
            res.status = 409;
            res.set_content ("{\"ok\":false,\"error\":\"no metadata; press Send All\"}", "application/json");
            return;
        }
        std::string body = "{\"ok\":true,\"count\":" + std::to_string (set->stories.size ()) + ",\"stories\":[";
        for (size_t i = 0; i < set->stories.size (); ++i) {
            if (i)
                body += ',';
            const Story& s = set->stories[i];
            body += "{\"index\":" + std::to_string (s.index) + ",\"name\":";
            JsonEscape (s.name, body);
            body += ",\"elevation\":" + Num (s.elevation);
            body += ",\"height\":" + (s.hasHeight ? Num (s.height) : std::string ("null"));
            body += "}";
        }
        body += "]}";
        res.set_content (body, "application/json");
    });

    // ---- All-hits ("pierce") ray ------------------------------------------

    // /ray/all?o=x,y,z&d=x,y,z&max=&maxHits=   (JSON)
    // Every surface the ray passes through, sorted by distance. Back faces are
    // not culled, so each solid yields an entry and an exit hit (`enter`).
    server->Get ("/ray/all", [] (const httplib::Request& req, httplib::Response& res) {
        auto eng = RequireEngine (req, res);
        if (!eng)
            return;
        const auto o = ParseDoubles (req.get_param_value ("o"));
        const auto d = ParseDoubles (req.get_param_value ("d"));
        if (o.size () != 3 || d.size () != 3) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"need o=x,y,z & d=x,y,z\"}", "application/json");
            return;
        }
        const double maxD = req.has_param ("max") ? std::atof (req.get_param_value ("max").c_str ()) : 0.0;
        const size_t maxHits =
            req.has_param ("maxHits")
                ? static_cast<size_t> (std::strtoul (req.get_param_value ("maxHits").c_str (), nullptr, 10))
                : 64;
        const double org[3] = { o[0], o[1], o[2] };
        const double dir[3] = { d[0], d[1], d[2] };

        const auto r = eng->RaycastAll (org, dir, maxD, maxHits);
        std::string body = "{\"ok\":true,\"count\":" + std::to_string (r.hits.size ()) +
                           ",\"truncated\":" + (r.truncated ? "true" : "false") + ",\"hits\":[";
        for (size_t i = 0; i < r.hits.size (); ++i) {
            if (i)
                body += ',';
            const auto& h = r.hits[i];
            body += "{\"guid\":\"" + eng->MeshGuid (h.meshIndex) + "\"";
            body += ",\"type\":" + std::to_string (eng->MeshType (h.meshIndex));
            body += ",\"t\":" + Num (h.t);
            body += ",\"enter\":" + std::string (h.enter ? "true" : "false");
            body += ",\"point\":" + Vec3Json (h.point);
            body += ",\"normal\":" + Vec3Json (h.normal);
            body += "}";
        }
        body += "]}";
        res.set_content (body, "application/json");
    });

    // POST /ray/all/batch  (binary, ragged/CSR — hit counts vary per ray)
    //   Request : int32 N, float64 maxDist, int32 maxHits,
    //             float64 origins[N*3], float64 dirs[N*3]
    //   Response: int32 N, int32 counts[N], uint8 truncated[N],
    //             then M = sum(counts):
    //             float64 t[M], int32 elem[M], uint8 enter[M], float32 normal[M*3]
    //   point = origin + t * normalize(dir)   (derive client-side)
    server->Post ("/ray/all/batch", [] (const httplib::Request& req, httplib::Response& res) {
        auto eng = RequireEngine (req, res);
        if (!eng)
            return;
        size_t off = 0;
        int32_t n = 0, maxHits = 0;
        double maxD = 0.0;
        if (!GetLE (req.body, off, n) || !GetLE (req.body, off, maxD) || !GetLE (req.body, off, maxHits) || n < 0 ||
            req.body.size () < off + static_cast<size_t> (n) * 6 * sizeof (double)) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"bad batch body\"}", "application/json");
            return;
        }
        const double* org = reinterpret_cast<const double*> (req.body.data () + off);
        const double* dir = org + static_cast<size_t> (n) * 3;
        const size_t cap = (maxHits > 0) ? static_cast<size_t> (maxHits) : 64;

        std::string countsB, truncB, tB, elemB, enterB, normB;
        countsB.reserve (n * 4);
        truncB.reserve (n);
        for (int32_t i = 0; i < n; ++i) {
            const auto r = eng->RaycastAll (&org[i * 3], &dir[i * 3], maxD, cap);
            PutLE<int32_t> (countsB, static_cast<int32_t> (r.hits.size ()));
            PutLE<uint8_t> (truncB, r.truncated ? 1 : 0);
            for (const auto& h : r.hits) {
                PutLE<double> (tB, h.t);
                PutLE<int32_t> (elemB, static_cast<int32_t> (h.meshIndex));
                PutLE<uint8_t> (enterB, h.enter ? 1 : 0);
                for (int k = 0; k < 3; ++k)
                    PutLE<float> (normB, static_cast<float> (h.normal[k]));
            }
        }
        std::string body;
        PutLE<int32_t> (body, n);
        body += countsB;
        body += truncB;
        body += tB;
        body += elemB;
        body += enterB;
        body += normB;
        res.set_content (body, "application/octet-stream");
    });

    // ---- Horizontal slice --------------------------------------------------
    // /slice?z=<m>[&types=1,2][&guids=A,B][&weld=1e-6]   (JSON)
    // Per-element polylines at the cut plane. Open chains are returned with
    // closed=false rather than dropped. Holes appear as extra loops.
    server->Get ("/slice", [] (const httplib::Request& req, httplib::Response& res) {
        auto snap = RequireSnapshot (req, res);
        if (!snap)
            return;
        if (!req.has_param ("z")) {
            res.status = 400;
            res.set_content ("{\"ok\":false,\"error\":\"need z=<meters>\"}", "application/json");
            return;
        }
        const double z = std::atof (req.get_param_value ("z").c_str ());
        const double weld = req.has_param ("weld") ? std::atof (req.get_param_value ("weld").c_str ()) : 1e-6;

        std::vector<int32_t> types;
        for (double v : ParseDoubles (req.get_param_value ("types")))
            types.push_back (static_cast<int32_t> (v));

        std::vector<std::string> guids;
        if (req.has_param ("guids")) {
            const std::string g = req.get_param_value ("guids");
            size_t i = 0;
            while (i < g.size ()) {
                size_t j = g.find (',', i);
                if (j == std::string::npos)
                    j = g.size ();
                if (j > i)
                    guids.push_back (g.substr (i, j - i));
                i = j + 1;
            }
        }

        const bool nudge = !(req.has_param ("nudge") && req.get_param_value ("nudge") == "0");

        const auto r = SliceZ (*snap, z, types, guids, weld, nudge);
        const auto& slices = r.elements;
        size_t nLoops = 0;
        for (const auto& s : slices)
            nLoops += s.loops.size ();

        std::string body = "{\"ok\":true,\"z\":" + Num (z) + ",\"zUsed\":" + Num (r.zUsed) +
                           ",\"nudged\":" + std::string (r.nudged ? "true" : "false") +
                           ",\"elementCount\":" + std::to_string (slices.size ()) +
                           ",\"loopCount\":" + std::to_string (nLoops) + ",\"elements\":[";
        for (size_t i = 0; i < slices.size (); ++i) {
            if (i)
                body += ',';
            const auto& s = slices[i];
            body += "{\"guid\":\"" + s.guid + "\",\"type\":" + std::to_string (s.elemType) + ",\"loops\":[";
            for (size_t l = 0; l < s.loops.size (); ++l) {
                if (l)
                    body += ',';
                const auto& pl = s.loops[l];
                body += "{\"closed\":" + std::string (pl.closed ? "true" : "false") + ",\"pts\":[";
                for (size_t p = 0; p < pl.PointCount (); ++p) {
                    if (p)
                        body += ',';
                    body +=
                        "[" + Num (pl.pts[p * 3]) + "," + Num (pl.pts[p * 3 + 1]) + "," + Num (pl.pts[p * 3 + 2]) + "]";
                }
                body += "]}";
            }
            body += "]}";
        }
        body += "]}";
        res.set_content (body, "application/json");
    });
}

bool HttpServer::Start (const char* host, int port)
{
    if (running.load ())
        return true;

    RegisterRoutes ();
    if (!server->bind_to_port (host, port)) {
        boundPort = 0;
        ServerState::Get ().serverRunning.store (false);
        ServerState::Get ().port.store (0);
        return false;
    }

    boundPort = port;
    running.store (true);
    ServerState::Get ().serverRunning.store (true);
    ServerState::Get ().port.store (boundPort);
    listenThread = std::thread ([this] {
        server->listen_after_bind (); // blocks until Stop()
        running.store (false);
        ServerState::Get ().serverRunning.store (false);
        ServerState::Get ().port.store (0);
    });
    return true;
}

void HttpServer::Stop ()
{
    // Stop the listener before joining its thread. The route workers use the same
    // dispatcher gate as external commands, so shutdown is performed before the
    // add-on removes its main-thread handlers in FreeData.
    if (server)
        server->stop ();
    if (listenThread.joinable ())
        listenThread.join ();
    running.store (false);
    ServerState::Get ().serverRunning.store (false);
    ServerState::Get ().port.store (0);
    QueryIndexCache::Get ().Release ();
    MeshStore::Get ().Release ();
    MetadataStore::Get ().Release ();
    ScreenshotStore::Get ().Release ();
    ServerState::Get ().snapshotId.store (0);
}

HttpServer& SharedHttpServer ()
{
    static HttpServer server;
    return server;
}

void ShutdownSharedHttpServer ()
{
    SharedHttpServer ().Stop ();
}

} // namespace geomsrv
