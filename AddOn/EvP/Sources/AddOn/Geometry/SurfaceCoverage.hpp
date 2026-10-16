#ifndef GEOMETRYSERVER_SURFACECOVERAGE_HPP
#define GEOMETRYSERVER_SURFACECOVERAGE_HPP

#include "GeometryEngine.hpp"
#include "Mesh.hpp"
#include <clipper2/clipper.h>

// Shared plane-local contact section from MassingFacade. No camera or SDK state.
namespace geomsrv::surfacecoverage {
using V = engine::Vector3;
constexpr double kPlaneTolerance = 1e-7;
constexpr int kPrecision = 7;

struct Frame {
    V origin, u, v, normal;
    Clipper2Lib::PointD Project (const V& point) const;
    V Lift (const Clipper2Lib::PointD& point) const;
};

bool MakeFrame (const V& a, const V& b, const V& c, Frame& frame);
bool Boolean (Clipper2Lib::ClipType operation, const Clipper2Lib::PathsD& subject, const Clipper2Lib::PathsD& clip,
              Clipper2Lib::PathsD& paths, std::string& error,
              Clipper2Lib::FillRule fill = Clipper2Lib::FillRule::NonZero);
// Input is a validated body. The exterior-side limit assigns coplanar vertices
// below the plane: opposite touching solids cover, bodies behind the face do not.
// Returns normalized section paths, holes retained. Open cuts refuse atomically.
bool Section (const Mesh& body, const Frame& frame, Clipper2Lib::PathsD& paths, std::string& error);
} // namespace geomsrv::surfacecoverage
#endif
