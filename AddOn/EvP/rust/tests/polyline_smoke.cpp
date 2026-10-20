#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustFfi.h"

#include <iostream>

int main ()
{
    static_assert (sizeof (TapiocaPolylineRequest) == 32);
    static_assert (offsetof (TapiocaPolylineRequest, parameters) == 16);
    static_assert (sizeof (TapiocaPolylineOutput) == 48);
    static_assert (offsetof (TapiocaPolylineOutput, totalLength) == 40);
    using namespace tapioca::geometry;
    const std::vector<double> points { 0, 0, 0, 0, 0, 8 }, parameters { 0, 0.5, 1 };
    PolylineSamples result;
    std::string error;
    if (!SamplePolyline (points, parameters, result, error) || !error.empty () || result.totalLength != 8 ||
        result.cumulative != std::vector<double> { 0, 8 } ||
        result.points != std::vector<double> { 0, 0, 0, 0, 0, 4, 0, 0, 8 } ||
        result.tangents != std::vector<double> { 0, 0, 1, 0, 0, 1, 0, 0, 1 })
        return 1;
    const auto before = result.points;
    if (SamplePolyline ({}, parameters, result, error) || result.points != before || error.empty ())
        return 2;
    if (!SamplePolyline (points, {}, result, error) || result.totalLength != 8 || !result.points.empty ())
        return 3;
    static_assert (sizeof (TapiocaDividePolylineRequest) == 24);
    static_assert (offsetof (TapiocaDividePolylineRequest, count) == 16);
    static_assert (offsetof (TapiocaDividePolylineRequest, includeEnds) == 20);
    std::vector<double> divided;
    if (!DividePolyline (points, 2, true, divided, error) ||
        divided != std::vector<double> { 0, 0, 0, 0, 0, 4, 0, 0, 8 })
        return 4;
    if (!DividePolyline (divided, 1, false, divided, error) || !divided.empty ())
        return 5;
    std::cout << "Tapioca polyline borrow/publication smoke passed\n";
    return 0;
}
