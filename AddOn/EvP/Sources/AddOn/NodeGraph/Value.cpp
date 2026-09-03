#include "NodeGraph/Value.hpp"

#include <algorithm>
#include <functional>
#include <type_traits>
#include <vector>

namespace evp::nodegraph {
namespace {

void CombineHash (size_t& seed, size_t value)
{
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

size_t HashPoint (const Point3& point)
{
    size_t hash = std::hash<double> {}(point.x);
    CombineHash (hash, std::hash<double> {}(point.y));
    CombineHash (hash, std::hash<double> {}(point.z));
    return hash;
}

} // namespace

ValueType Value::Type () const
{
    return static_cast<ValueType> (data_.index ());
}

size_t Value::Hash () const
{
    size_t hash = data_.index ();
    std::visit (
        [&hash] (const auto& value) {
            using T = std::decay_t<decltype (value)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return;
            }
            else if constexpr (std::is_same_v<T, Point3>) {
                CombineHash (hash, HashPoint (value));
            }
            else if constexpr (std::is_same_v<T, Polyline> || std::is_same_v<T, Polygon>) {
                for (const Point3& point : value.points)
                    CombineHash (hash, HashPoint (point));
            }
            else if constexpr (std::is_same_v<T, ImmutableMesh>) {
                CombineHash (hash, std::hash<const geomsrv::Mesh*> {}(value.get ()));
            }
            else if constexpr (std::is_same_v<T, ArchicadElementRef>) {
                CombineHash (hash, std::hash<std::string> {}(value.guid));
            }
            else {
                CombineHash (hash, std::hash<T> {}(value));
            }
        },
        data_);
    return hash;
}

size_t Argument::Hash () const
{
    if (!isList_)
        return item_.Hash ();
    size_t hash = std::hash<size_t> {}(items_.size ());
    for (const Value& item : items_)
        CombineHash (hash, item.Hash ());
    return hash;
}

} // namespace evp::nodegraph
