#pragma once
#include "ArchViz/LayoutReportStudy.hpp"
#include "ArchViz/ShadowCaptureArea.hpp"
#include "ObjScene.hpp"
#include "SunVisibility.hpp"
#include "SunStudy/SunStudyRustFfi.h"
#include <stdexcept>

namespace renderbench {
// Fixture-only geometry binding. Feed the production accumulator and tint atlas,
// not pixel classifications. Hours are local apparent solar time at 52 N;
// directions are explicit fixture inputs, not an alternative project ephemeris.
struct SeasonalReportFixture {
    static constexpr unsigned side = 200, plane = side * side;
    geomsrv::archviz::SunStudyElementMap map;
    SunVisibility bvh;
    std::vector<double> points, areas;
    std::vector<size_t> texels;
    void Bind (const ObjScene& obj)
    {
        bvh.Build (obj);
        for (const auto& e : obj.elements)
            if (obj.materialNames.at (e->ranges[0].material) == "Ground") {
                map.guid = e->guid;
                map.topologyHash = geomsrv::archviz::MeshIndexHash (e->indices);
                map.faces.resize (e->indices.size () / 3);
                for (auto& f : map.faces) {
                    f.originAndInvSpacing[0] = f.originAndInvSpacing[1] = -20;
                    f.originAndInvSpacing[3] = 5;
                    f.uAxisAndStart[0] = f.vAxisAndStart[1] = 1;
                    f.tile[2] = f.tile[3] = side;
                }
            }
        if (map.faces.size () != 2)
            throw std::runtime_error ("use the courtyard fixture with its Ground receiver");
        const double up[] = { 0, 0, 1 };
        for (unsigned j = 0; j < side; ++j)
            for (unsigned i = 0; i < side; ++i) {
                double p[] = { -20 + (i + .5) * .2, -20 + (j + .5) * .2, .005 };
                if (bvh.Blocked (p, up, .001))
                    continue; // remove building footprints from the receiver
                points.insert (points.end (), p, p + 3);
                areas.push_back (.04);
                texels.push_back (j * side + i);
            }
    }
    geomsrv::archviz::SunStudyAtlasUpload Day (unsigned row)
    {
        geomsrv::archviz::SunStudyAtlasUpload p;
        p.studyId = "courtyard-" + std::to_string (row);
        p.version = 1;
        p.width = p.height = side;
        p.quantumHours = .25f;
        p.elements = { map };
        p.sampleCount = areas.size ();
        constexpr double rad = 3.141592653589793 / 180;
        double lat = 52 * rad, dec = (row == 0 ? 0 : row == 1 ? 23.44 : -23.44) * rad;
        std::vector<double> directions;
        for (int m = 0; m < 1440; m += 15) {
            double t = (m / 60. - 12) * 15 * rad,
                   sun[] = { -std::cos (dec) * std::sin (t),
                             std::cos (lat) * std::sin (dec) - std::sin (lat) * std::cos (dec) * std::cos (t),
                             std::sin (lat) * std::sin (dec) + std::cos (lat) * std::cos (dec) * std::cos (t) };
            if (sun[2] < .00001)
                continue;
            if (m < 720)
                ++p.noonStep;
            directions.insert (directions.end (), sun, sun + 3);
            p.stepMinutes.push_back (uint16_t (m));
            p.stepSuns.push_back ({ float (sun[0]), float (sun[1]), float (sun[2]) });
        }
        p.stepCount = unsigned (p.stepMinutes.size ());
        p.stepWords = (p.stepCount + 31) / 32;
        p.hoursMax = p.stepCount * .25f;
        char error[512] = {};
        TapiocaSunStudyOcclusion* handle = nullptr;
        if (tapioca_sunstudy_occlusion_create (areas.size (), p.stepCount, &handle, error, sizeof (error)))
            throw std::runtime_error (error);
        TapiocaTraversalVtable tree {};
        tree.context = &bvh;
        tree.sceneVersion = [] (void*) -> uint64_t { return 1; };
        tree.occludeDirectional = [] (void* ctx, const double* xyz, size_t n, const double* dir, double near, double,
                                      uint8_t* hits, size_t) -> uint8_t {
            for (size_t i = 0; i < n; ++i)
                hits[i] = static_cast<SunVisibility*> (ctx)->Blocked (xyz + 3 * i, dir, near);
            return 1;
        };
        TapiocaOcclusionRangeRequest request {};
        request.trace.traversal = &tree;
        request.trace.positions = { points.data (), points.size () };
        request.trace.sampleCount = areas.size ();
        request.trace.tmin = .001;
        request.directions = { directions.data (), directions.size () };
        request.maxSteps = p.stepCount;
        size_t done = 0;
        const size_t words = (p.stepCount + 63) / 64;
        std::vector<uint64_t> bits (areas.size () * words);
        std::vector<double> hours (areas.size ());
        TapiocaOcclusionReadOutput out {};
        out.bits = bits.data ();
        out.bitCapacity = bits.size ();
        out.sunHours = hours.data ();
        out.hoursCapacity = hours.size ();
        out.hoursPerStep = .25;
        bool ok = !tapioca_sunstudy_occlusion_range (handle, &request, &done, error, sizeof (error)) &&
                  done == p.stepCount && !tapioca_sunstudy_occlusion_read (handle, &out, error, sizeof (error));
        tapioca_sunstudy_occlusion_destroy (handle, nullptr, 0);
        if (!ok)
            throw std::runtime_error (error);
        auto masks = std::make_shared<std::vector<uint32_t>> (size_t (plane) * p.stepWords);
        auto values = std::make_shared<std::vector<float>> (plane, -1);
        for (size_t i = 0; i < areas.size (); ++i) {
            (*values)[texels[i]] = float (hours[i]);
            for (unsigned w = 0; w < p.stepWords; ++w)
                (*masks)[plane * w + texels[i]] = uint32_t (bits[i * words + w / 2] >> (32 * (w % 2)));
        }
        p.texels = values;
        p.stepMasks = masks;
        return p;
    }
};
} // namespace renderbench
