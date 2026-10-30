#ifndef EVP_EDIT_EDITTYPES_HPP
#define EVP_EDIT_EDITTYPES_HPP

// The editor's value types: a point or vector in world metres and the id every
// editable item carries. Shared by the document and by the definitions it holds.
#include <cstdint>

namespace geomsrv::edit {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
Vec3 operator+ (Vec3 a, Vec3 b);
Vec3 operator- (Vec3 a, Vec3 b);
Vec3 operator* (Vec3 a, double s);
bool operator== (Vec3 a, Vec3 b);
double Dot (Vec3 a, Vec3 b);
Vec3 Cross (Vec3 a, Vec3 b);
double Length (Vec3 a);
double Distance (Vec3 a, Vec3 b);
double DistanceXY (Vec3 a, Vec3 b);
Vec3 Lerp (Vec3 a, Vec3 b, double t);
// A zero or non-finite input returns false and leaves `out` untouched.
bool Normalize (Vec3 a, Vec3& out);
bool Finite (Vec3 a);

using Id = uint64_t;
constexpr Id kNoId = 0;

// Where an object sits in the editor's own organisation (EditOrganize.hpp): its layer and
// innermost group (kNoId: none), and whether it is hidden or locked itself. A view state
// only: hiding or locking never changes the geometry or what Accept writes.
struct Membership {
    Id layer = kNoId, group = kNoId;
    bool hidden = false, locked = false;
    bool operator== (const Membership&) const = default;
};

} // namespace geomsrv::edit
#endif
