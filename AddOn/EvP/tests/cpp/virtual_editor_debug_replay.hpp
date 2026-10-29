#ifndef TAPIOCA_TEST_VIRTUAL_EDITOR_DEBUG_REPLAY_HPP
#define TAPIOCA_TEST_VIRTUAL_EDITOR_DEBUG_REPLAY_HPP

// Offline diagnostic reader: schema-check authored values without persistence's
// region/geometry acceptance gate. Broken geometry is exactly what a dump preserves.
#include "Edit/EditLandscapeJsonInternal.hpp"
#include "Edit/EditTerrainCapture.hpp"
#include "Edit/EditTerrainDesignJob.hpp"
#include "Edit/EditRegionTerrain.hpp"
#include "Geometry/Mesh.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace debugreplay {
namespace lj = tapioca::geometry::landscape_json;
namespace lg = tapioca::geometry;
namespace js = evp::nodegraph::json;

template <class T> std::vector<T> Buffer (const lj::Reader& reader)
{
    const auto* values = reader.value.AsArray ();
    if (!values || values->size () > 32000000)
        throw std::runtime_error (reader.path + ": bounded numeric buffer required");
    std::vector<T> result;
    result.reserve (values->size ());
    for (const auto& value : *values) {
        const auto number = lj::Reader (value, reader.path).Number ();
        if constexpr (std::numeric_limits<T>::is_integer)
            if (number != std::floor (number) || number < std::numeric_limits<T>::lowest () ||
                number > std::numeric_limits<T>::max ())
                throw std::runtime_error (reader.path + ": integer buffer overflow");
        result.push_back (static_cast<T> (number));
    }
    return result;
}

struct Scene {
    geomsrv::edit::Document doc;
    geomsrv::Mesh baseline;
    lg::TerrainDesignOptions options;
    std::vector<double> region;
    bool nativeSolid = false;
};

inline Scene Read (const js::JsonValue& value)
{
    const lj::Reader root (value, "debug");
    if (root.Field ("format").Text () != "tapioca.virtual-editor.debug" || root.Field ("version").Integer () != 1)
        throw std::runtime_error ("Unsupported virtual editor dump format/version");
    Scene scene;
    const auto document = root.Field ("document");
    if (document.Field ("version").Integer () != 1 || document.Field ("mode").Text () != "virtual")
        throw std::runtime_error ("Expected a schema-v1 virtual document");
    lj::ReadDocument (document, scene.doc, lg::LandscapeStorage::Virtual);
    if (!root.Field ("hadLandscape").Flag ())
        scene.doc.landscape.reset ();
    const auto baseline = root.Field ("baseline");
    if (!baseline.value.IsNull ()) {
        scene.baseline.guid = baseline.Field ("guid").Text ();
        scene.baseline.elemType = baseline.Field ("elemType").Integer ();
        scene.baseline.vertices = Buffer<double> (baseline.Field ("points"));
        scene.baseline.triangles = Buffer<uint32_t> (baseline.Field ("triangles"));
        scene.baseline.triSourceBody = Buffer<int32_t> (baseline.Field ("triSourceBody"));
        scene.baseline.triSourcePgon = Buffer<int32_t> (baseline.Field ("triSourcePgon"));
        scene.baseline.triMaterial = Buffer<int32_t> (baseline.Field ("triMaterial"));
        scene.baseline.triSurfaceAttribute = Buffer<int32_t> (baseline.Field ("triSurfaceAttribute"));
    }
    scene.nativeSolid = root.Field ("nativeSolid").Flag ();
    auto& o = scene.options;
    const auto options = root.Field ("designOptions");
    o.operation = options.Field ("operation").Identifier ();
    o.object = options.Field ("object").Identifier ();
    o.source = options.Field ("source").Identifier ();
    o.outline = options.Field ("outline").Flag ();
    o.trimToCover = options.Field ("trimToCover").Flag ();
    o.trimMargin = options.Field ("trimMargin").Number ();
    o.margin = options.Field ("margin").Number ();
    o.arcTolerance = options.Field ("arcTolerance").Number ();
    o.maxStep = options.Field ("maxStep").Number ();
    o.taper = options.Field ("taper").Number ();
    o.cutLimit = options.Field ("cutLimit").Number ();
    o.fillLimit = options.Field ("fillLimit").Number ();
    const int effect = options.Field ("effect").Integer ();
    if (effect < 1 || effect > 3)
        throw std::runtime_error ("Invalid terrain edit effect");
    o.effect = lg::TerrainEditEffect (effect);
    o.tolerance = options.Field ("tolerance").Number ();
    o.designTable = options.Field ("designTable").Identifier ();
    o.meshTable = options.Field ("meshTable").Identifier ();
    o.cover = Buffer<double> (root.Field ("designCover"));
    scene.region = Buffer<double> (root.Field ("designRegion"));
    return scene;
}

#if defined(TAPIOCA_GEOMETRY_RUST)
inline lg::TerrainDesignOutcome Run (const Scene& scene)
{
    auto capture = std::make_shared<lg::TerrainCapture> ();
    std::string error;
    if (!lg::CaptureLandscapeTerrain (scene.doc, scene.baseline, { scene.options.object, 2, 1 }, *capture, error))
        throw std::runtime_error (error);
    lg::TerrainDesignJob job;
    job.doc = std::make_shared<const geomsrv::edit::Document> (scene.doc);
    job.capture = capture;
    job.options = scene.options;
    job.region = scene.region;
    job.surfaceTable = scene.nativeSolid ? 5 : 0;
    return lg::RunLandscapeTerrainDesign (job);
}
#endif
} // namespace debugreplay
#endif
