#include "NodeGraph/Data/AtomicValue.hpp"

#include <cstring>
#include <functional>

namespace evp::nodegraph::data {
namespace {

size_t HashPoint (const Point3& point)
{
    size_t hash = std::hash<double> {}(point.x);
    CombineItemHash (hash, std::hash<double> {}(point.y));
    CombineItemHash (hash, std::hash<double> {}(point.z));
    return hash;
}

bool SamePoint (const Point3& left, const Point3& right)
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

size_t HashPoints (const std::vector<Point3>& points)
{
    size_t hash = points.size ();
    for (const Point3& point : points)
        CombineItemHash (hash, HashPoint (point));
    return hash;
}

bool SamePoints (const std::vector<Point3>& left, const std::vector<Point3>& right)
{
    if (left.size () != right.size ())
        return false;
    for (size_t index = 0; index < left.size (); ++index) {
        if (!SamePoint (left[index], right[index]))
            return false;
    }
    return true;
}

} // namespace

void CombineItemHash (size_t& seed, size_t value)
{
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

const char* ItemTypeName (ItemType type)
{
    switch (type) {
        case ItemType::Bool:
            return "bool";
        case ItemType::Integer:
            return "integer";
        case ItemType::Double:
            return "double";
        case ItemType::String:
            return "string";
        case ItemType::Point3:
            return "point3";
        case ItemType::Polyline:
            return "polyline";
        case ItemType::Polygon:
            return "polygon";
        case ItemType::Mesh:
            return "mesh";
        case ItemType::ElementRef:
            return "elementRef";
        case ItemType::Any:
            return "any";
    }
    return "unknown";
}

std::optional<ItemType> ItemTypeFromName (std::string_view name)
{
    static constexpr ItemType kAll[] = { ItemType::Bool,       ItemType::Integer,  ItemType::Double,  ItemType::String,
                                         ItemType::Point3,     ItemType::Polyline, ItemType::Polygon, ItemType::Mesh,
                                         ItemType::ElementRef, ItemType::Any };
    for (ItemType type : kAll) {
        if (name == ItemTypeName (type))
            return type;
    }
    return std::nullopt;
}

std::optional<ItemType> ItemTypeFromValueType (ValueType type)
{
    switch (type) {
        case ValueType::Bool:
            return ItemType::Bool;
        case ValueType::Integer:
            return ItemType::Integer;
        case ValueType::Double:
            return ItemType::Double;
        case ValueType::String:
            return ItemType::String;
        case ValueType::Point3:
            return ItemType::Point3;
        case ValueType::Polyline:
            return ItemType::Polyline;
        case ValueType::Polygon:
            return ItemType::Polygon;
        case ValueType::Mesh:
            return ItemType::Mesh;
        case ValueType::ArchicadElementRef:
            return ItemType::ElementRef;
        case ValueType::Absent:
        case ValueType::List:
            // Deliberate: a tree expresses absence as a null cell (HANDOFF 7.5)
            // and collection shape as its own topology (7.2). Neither is an item.
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<ValueType> ValueTypeFromItemType (ItemType type)
{
    switch (type) {
        case ItemType::Bool:
            return ValueType::Bool;
        case ItemType::Integer:
            return ValueType::Integer;
        case ItemType::Double:
            return ValueType::Double;
        case ItemType::String:
            return ValueType::String;
        case ItemType::Point3:
            return ValueType::Point3;
        case ItemType::Polyline:
            return ValueType::Polyline;
        case ItemType::Polygon:
            return ValueType::Polygon;
        case ItemType::Mesh:
            return ValueType::Mesh;
        case ItemType::ElementRef:
            return ValueType::ArchicadElementRef;
        case ItemType::Any:
            return std::nullopt;
    }
    return std::nullopt;
}

bool IsAtomicValue (const Value& value)
{
    const ValueType type = value.Type ();
    return type != ValueType::Absent && type != ValueType::List;
}

Value ToValue (bool item)
{
    return Value (item);
}

Value ToValue (int64_t item)
{
    return Value (item);
}

Value ToValue (double item)
{
    return Value (item);
}

Value ToValue (const std::string& item)
{
    return Value (item);
}

Value ToValue (const Point3& item)
{
    return Value (item);
}

Value ToValue (const Polyline& item)
{
    return Value (item);
}

Value ToValue (const Polygon& item)
{
    return Value (item);
}

Value ToValue (const Value::ImmutableMesh& item)
{
    return Value (item);
}

Value ToValue (const ArchicadElementRef& item)
{
    return Value (item);
}

Value ToValue (const Value& item)
{
    return item;
}

size_t ItemTraits<bool>::Hash (const bool& value)
{
    return std::hash<bool> {}(value);
}

bool ItemTraits<bool>::Equals (const bool& left, const bool& right)
{
    return left == right;
}

size_t ItemTraits<int64_t>::Hash (const int64_t& value)
{
    return std::hash<int64_t> {}(value);
}

bool ItemTraits<int64_t>::Equals (const int64_t& left, const int64_t& right)
{
    return left == right;
}

size_t ItemTraits<double>::Hash (const double& value)
{
    return std::hash<double> {}(value);
}

bool ItemTraits<double>::Equals (const double& left, const double& right)
{
    // Bitwise, not ==: two NaN items are the same item as far as caching is
    // concerned, and +0.0/-0.0 hash differently under std::hash<double>, so
    // comparing those two equal would break the hash/equality agreement.
    return std::memcmp (&left, &right, sizeof (double)) == 0;
}

size_t ItemTraits<std::string>::Hash (const std::string& value)
{
    return std::hash<std::string> {}(value);
}

bool ItemTraits<std::string>::Equals (const std::string& left, const std::string& right)
{
    return left == right;
}

size_t ItemTraits<Point3>::Hash (const Point3& value)
{
    return HashPoint (value);
}

bool ItemTraits<Point3>::Equals (const Point3& left, const Point3& right)
{
    return SamePoint (left, right);
}

size_t ItemTraits<Polyline>::Hash (const Polyline& value)
{
    return HashPoints (value.points);
}

bool ItemTraits<Polyline>::Equals (const Polyline& left, const Polyline& right)
{
    return SamePoints (left.points, right.points);
}

size_t ItemTraits<Polygon>::Hash (const Polygon& value)
{
    return HashPoints (value.points);
}

bool ItemTraits<Polygon>::Equals (const Polygon& left, const Polygon& right)
{
    return SamePoints (left.points, right.points);
}

size_t ItemTraits<Value::ImmutableMesh>::Hash (const Value::ImmutableMesh& value)
{
    return std::hash<const geomsrv::Mesh*> {}(value.get ());
}

bool ItemTraits<Value::ImmutableMesh>::Equals (const Value::ImmutableMesh& left, const Value::ImmutableMesh& right)
{
    return left.get () == right.get ();
}

size_t ItemTraits<ArchicadElementRef>::Hash (const ArchicadElementRef& value)
{
    return std::hash<std::string> {}(value.guid);
}

bool ItemTraits<ArchicadElementRef>::Equals (const ArchicadElementRef& left, const ArchicadElementRef& right)
{
    return left.guid == right.guid;
}

size_t ItemTraits<Value>::Hash (const Value& value)
{
    return value.Hash ();
}

bool ItemTraits<Value>::Equals (const Value& left, const Value& right)
{
    const ValueType type = left.Type ();
    if (type != right.Type ())
        return false;
    if (!IsAtomicValue (left))
        return false; // See IsAtomicValue: undefined for values a tree cannot hold.

    switch (type) {
        case ValueType::Bool:
            return std::get<bool> (left.DataValue ()) == std::get<bool> (right.DataValue ());
        case ValueType::Integer:
            return std::get<int64_t> (left.DataValue ()) == std::get<int64_t> (right.DataValue ());
        case ValueType::Double:
            return ItemTraits<double>::Equals (std::get<double> (left.DataValue ()),
                                               std::get<double> (right.DataValue ()));
        case ValueType::String:
            return std::get<std::string> (left.DataValue ()) == std::get<std::string> (right.DataValue ());
        case ValueType::Point3:
            return SamePoint (std::get<Point3> (left.DataValue ()), std::get<Point3> (right.DataValue ()));
        case ValueType::Polyline:
            return SamePoints (std::get<Polyline> (left.DataValue ()).points,
                               std::get<Polyline> (right.DataValue ()).points);
        case ValueType::Polygon:
            return SamePoints (std::get<Polygon> (left.DataValue ()).points,
                               std::get<Polygon> (right.DataValue ()).points);
        case ValueType::Mesh:
            return std::get<Value::ImmutableMesh> (left.DataValue ()).get () ==
                   std::get<Value::ImmutableMesh> (right.DataValue ()).get ();
        case ValueType::ArchicadElementRef:
            return std::get<ArchicadElementRef> (left.DataValue ()).guid ==
                   std::get<ArchicadElementRef> (right.DataValue ()).guid;
        case ValueType::Absent:
        case ValueType::List:
            return false;
    }
    return false;
}

} // namespace evp::nodegraph::data
