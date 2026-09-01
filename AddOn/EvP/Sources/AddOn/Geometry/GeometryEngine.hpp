#ifndef EVP_GEOMETRY_GEOMETRYENGINE_HPP
#define EVP_GEOMETRY_GEOMETRYENGINE_HPP

#include <string>
#include <vector>

namespace geomsrv::engine {

struct Vector3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Polygon {
    std::vector<Vector3> points;
};

enum class PolygonOperation {
    Union,
    Difference,
    Intersection,
};

Vector3 Add (const Vector3& left, const Vector3& right);
Vector3 Cross (const Vector3& left, const Vector3& right);
double Dot (const Vector3& left, const Vector3& right);
bool Unit (const Vector3& value, Vector3& result, std::string& error);

bool BooleanPolygons (const Polygon& subject, const Polygon& clip, PolygonOperation operation,
                      std::vector<Polygon>& result, std::string& error);
bool OffsetPolygon (const Polygon& polygon, double distance, std::vector<Polygon>& result, std::string& error);

} // namespace geomsrv::engine

#endif
