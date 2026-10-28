#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "AddOnVersion.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditTerrainDesignJob.hpp"
#include "Geometry/Mesh.hpp"
#include "NodeGraph/Json.hpp"
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace geomsrv::archviz::edithud {
namespace {
namespace js = evp::nodegraph::json;
using V = js::JsonValue;
V Id (uint64_t value)
{
    return V::String (std::to_string (value));
}

// Terrain buffers can be much larger than the authored document. Stream them
// directly instead of allocating a JSON value per coordinate/index.
template <class Range> void Numbers (std::ostream& out, const Range& values)
{
    out << '[';
    bool first = true;
    for (auto value : values) {
        if (!std::isfinite (static_cast<double> (value)))
            throw std::runtime_error ("A mesh buffer contains a non-finite number.");
        if (!first)
            out << ',';
        first = false;
        out << +value;
    }
    out << ']';
}
void Options (std::ostream& out, const tapioca::geometry::TerrainDesignOptions& o)
{
    out << "{\"operation\":" << js::Write (Id (o.operation), 0) << ",\"object\":" << js::Write (Id (o.object), 0)
        << ",\"source\":" << js::Write (Id (o.source), 0) << ",\"outline\":" << (o.outline ? "true" : "false")
        << ",\"trimToCover\":" << (o.trimToCover ? "true" : "false") << ",\"trimMargin\":" << o.trimMargin
        << ",\"margin\":" << o.margin << ",\"arcTolerance\":" << o.arcTolerance << ",\"maxStep\":" << o.maxStep
        << ",\"taper\":" << o.taper << ",\"cutLimit\":" << o.cutLimit << ",\"fillLimit\":" << o.fillLimit
        << ",\"effect\":" << int (o.effect) << ",\"tolerance\":" << o.tolerance
        << ",\"designTable\":" << js::Write (Id (o.designTable), 0)
        << ",\"meshTable\":" << js::Write (Id (o.meshTable), 0) << '}';
}
std::string Stamp ()
{
    const auto now = std::chrono::system_clock::now ();
    const auto time = std::chrono::system_clock::to_time_t (now);
    std::tm utc {};
#ifdef _WIN32
    gmtime_s (&utc, &time);
#else
    gmtime_r (&time, &utc);
#endif
    std::ostringstream out;
    out << std::put_time (&utc, "%Y%m%d_%H%M%S") << '_' << std::setfill ('0') << std::setw (6)
        << std::chrono::duration_cast<std::chrono::microseconds> (now.time_since_epoch ()).count () % 1000000 << 'Z';
    return out.str ();
}
bool Failed (Session& s, const std::string& error)
{
    s.debugExportRequested = false;
    s.debugExportError = error;
    s.message = "Debug export failed: " + error;
    ArchVizLog (s.message);
    return false;
}
} // namespace

bool RequestDebugExport (Session& s)
{
    if (s.debugExportRequested)
        return false;
    if (s.close || s.edits.InGesture () || !s.drawing.empty () || GizmoModeLocked (s) || s.landscapeCommand ||
        s.terrainCapture || s.draftingImport)
        return Failed (s, "Finish the current input or capture before exporting.");
    s.debugExportRequested = true;
    s.debugExportRevision = s.edits.Doc ().revision;
    s.debugExportError.clear ();
    return true;
}

bool EncodeDebugScene (const Session& s, std::string& text, std::string& error)
{
    try {
        const auto source = GetTerrainDebugSource (s);
        const auto status = GetTerrainDesignStatus (s);
        const auto* current = source.outcomeMatchesOptions ? CurrentTerrainDesign (s) : nullptr;
        std::ostringstream out;
        out.exceptions (std::ios::badbit | std::ios::failbit);
        out.imbue (std::locale::classic ());
        out << std::setprecision (std::numeric_limits<double>::max_digits10);
        out << "{\"format\":\"tapioca.virtual-editor.debug\",\"version\":1,\"units\":\"metres\",\"axes\":\"right-"
               "handed Z-up\",";
        out << "\"createdUtc\":" << js::Write (V::String (Stamp ()), 0)
            << ",\"addonVersion\":" << js::Write (V::String (ADDON_VERSION), 0)
            << ",\"compiled\":" << js::Write (V::String (__DATE__ " " __TIME__), 0)
            << ",\"hadLandscape\":" << (s.edits.Doc ().landscape ? "true" : "false")
            << ",\"document\":" << tapioca::geometry::WriteLandscapeDebugSnapshot (s.edits.Doc ())
            << ",\"terrainPath\":" << js::Write (Id (s.terrainPath), 0)
            << ",\"terrainBand\":" << (s.terrainBand ? "true" : "false")
            << ",\"contours\":" << (s.contours ? "true" : "false") << ",\"contourStep\":" << s.contourStep
            << ",\"baselineFresh\":" << (source.fresh ? "true" : "false")
            << ",\"nativeSolid\":" << (source.nativeSolid ? "true" : "false")
            << ",\"designActive\":" << (source.active ? "true" : "false") << ",\"designOptions\":";
        Options (out, source.options);
        out << ",\"designCover\":";
        Numbers (out, source.options.cover);
        out << ",\"designRegion\":";
        Numbers (out, source.options.region);
        out << ",\"status\":"
            << js::Write (V::Object ({ { "pending", V::Bool (status.pending) },
                                       { "current", V::Bool (current != nullptr) },
                                       { "built", V::Bool (current && current->built) },
                                       { "error", V::String (status.error) },
                                       { "message", V::String (s.message) } }),
                          0);
        out << ",\"baseline\":";
        if (source.mesh) {
            const auto& m = *source.mesh;
            out << "{\"guid\":" << js::Write (V::String (m.guid), 0) << ",\"elemType\":" << m.elemType
                << ",\"points\":";
            Numbers (out, m.vertices);
            out << ",\"triangles\":";
            Numbers (out, m.triangles);
            out << ",\"triSourceBody\":";
            Numbers (out, m.triSourceBody);
            out << ",\"triSourcePgon\":";
            Numbers (out, m.triSourcePgon);
            out << ",\"triMaterial\":";
            Numbers (out, m.triMaterial);
            out << ",\"triSurfaceAttribute\":";
            Numbers (out, m.triSurfaceAttribute);
            out << '}';
        }
        else
            out << "null";
        out << ",\"outcome\":";
        if (current) {
            js::JsonArray unmet;
            for (const auto& condition : current->unmet)
                unmet.push_back (V::String (condition));
            out << "{\"generation\":" << js::Write (Id (current->generation), 0)
                << ",\"revision\":" << js::Write (Id (current->revision), 0)
                << ",\"built\":" << (current->built ? "true" : "false")
                << ",\"error\":" << js::Write (V::String (current->error), 0)
                << ",\"unmet\":" << js::Write (V::Array (std::move (unmet)), 0) << ",\"points\":";
            Numbers (out, current->result.build.points);
            out << ",\"triangles\":";
            Numbers (out, current->result.build.triangles);
            out << ",\"baseHeights\":";
            Numbers (out, current->result.build.base);
            out << ",\"region\":";
            Numbers (out, current->result.region);
            out << ",\"stations\":";
            Numbers (out, current->result.stations);
            out << '}';
        }
        else
            out << "null"; // Never attach a previous revision's mesh to the current document.
        out << ",\"pavedCache\":{\"revision\":" << js::Write (Id (s.pavedRevision), 0) << ",\"step\":" << s.pavedStep
            << ",\"warning\":" << js::Write (V::String (s.paved.warning), 0) << ",\"points\":[";
        for (size_t i = 0; i < s.paved.vertices.size (); ++i) {
            if (i)
                out << ',';
            const auto p = s.paved.vertices[i];
            out << p.x << ',' << p.y << ',' << p.z;
        }
        out << "],\"triangles\":";
        Numbers (out, s.paved.triangles);
        out << ",\"rings\":[";
        for (size_t i = 0; i < s.paved.rings.size (); ++i) {
            if (i)
                out << ',';
            out << '[';
            for (size_t j = 0; j < s.paved.rings[i].size (); ++j) {
                if (j)
                    out << ',';
                const auto p = s.paved.rings[i][j];
                out << p.x << ',' << p.y << ',' << p.z;
            }
            out << ']';
        }
        out << ']';
        out << "}}\n";
        text = out.str ();
        error.clear ();
        return true;
    }
    catch (const std::exception& exception) {
        error = exception.what ();
        return false;
    }
}

bool SaveDebugExport (Session& s, const std::filesystem::path& directory)
{
    if (!s.debugExportRequested)
        return false;
    s.debugExportRequested = false;
    if (s.close || s.edits.Doc ().revision != s.debugExportRevision || s.edits.InGesture () || !s.drawing.empty () ||
        s.terrainCapture || s.draftingImport || GizmoModeLocked (s) || s.landscapeCommand)
        return Failed (s, "The editor changed before export; click Export debug scene again.");
    std::string text, error;
    if (!EncodeDebugScene (s, text, error))
        return Failed (s, error);
    try {
        std::filesystem::create_directories (directory);
        const auto stem = "virtual-editor__" + Stamp ();
        for (unsigned attempt = 0; attempt < 10000; ++attempt) {
            const auto path = directory / (stem + "_" + std::to_string (attempt) + ".json");
            FILE* file = nullptr;
            // Exclusive create is essential: two clicks/processes must never overwrite evidence.
#ifdef _WIN32
            const auto opened = _wfopen_s (&file, path.c_str (), L"wbx");
#else
            file = std::fopen (path.c_str (), "wbx");
            const auto opened = file ? 0 : errno;
#endif
            if (opened == EEXIST)
                continue;
            if (!file)
                return Failed (s, "Cannot create a dump file (error " + std::to_string (opened) + ").");
            const bool written = std::fwrite (text.data (), 1, text.size (), file) == text.size ();
            const bool closed = std::fclose (file) == 0;
            if (!written || !closed) {
                std::error_code ignored;
                std::filesystem::remove (path, ignored);
                return Failed (s, "Could not finish writing the dump file.");
            }
            const auto utf8 = path.u8string ();
            s.debugExportPath.assign (utf8.begin (), utf8.end ());
            s.debugExportError.clear ();
            s.message = "Debug scene saved: " + s.debugExportPath;
            ArchVizLog (s.message);
            return true;
        }
        return Failed (s, "Could not allocate a unique dump filename.");
    }
    catch (const std::exception& exception) {
        return Failed (s, exception.what ());
    }
}

bool SaveDebugExport (Session& s)
{
#ifdef _WIN32
    const auto* root = _wgetenv (L"LOCALAPPDATA");
#else
    const auto* root = std::getenv ("LOCALAPPDATA");
#endif
    if (!root || !*root)
        return Failed (s, "LOCALAPPDATA is unavailable.");
    return SaveDebugExport (s, std::filesystem::path (root) / "Tapioca" / "dumps");
}
} // namespace geomsrv::archviz::edithud
