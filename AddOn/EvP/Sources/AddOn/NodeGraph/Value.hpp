#ifndef EVP_NODEGRAPH_VALUE_HPP
#define EVP_NODEGRAPH_VALUE_HPP

#include "Annotation/DrawList.hpp"
#include "Geometry/Mesh.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace evp::nodegraph {

using Point3 = geomsrv::annotation::Point3;

struct Polyline {
    std::vector<Point3> points;
};

struct Polygon {
    std::vector<Point3> points;
};

struct ArchicadElementRef {
    std::string guid;
};

enum class ValueType {
    Absent,
    Bool,
    Integer,
    Double,
    String,
    Point3,
    Polyline,
    Polygon,
    Mesh,
    ArchicadElementRef,
    List,
};

class Value {
  public:
    using ImmutableMesh = std::shared_ptr<const geomsrv::Mesh>;
    using Data = std::variant<std::monostate, bool, int64_t, double, std::string, Point3, Polyline, Polygon,
                              ImmutableMesh, ArchicadElementRef>;

    Value () = default;
    explicit Value (bool value) : data_ (value)
    {
    }
    explicit Value (int64_t value) : data_ (value)
    {
    }
    explicit Value (double value) : data_ (value)
    {
    }
    explicit Value (std::string value) : data_ (std::move (value))
    {
    }
    explicit Value (const char* value) : data_ (std::string (value))
    {
    }
    explicit Value (Point3 value) : data_ (value)
    {
    }
    explicit Value (Polyline value) : data_ (std::move (value))
    {
    }
    explicit Value (Polygon value) : data_ (std::move (value))
    {
    }
    explicit Value (ImmutableMesh value) : data_ (std::move (value))
    {
    }
    explicit Value (ArchicadElementRef value) : data_ (std::move (value))
    {
    }

    ValueType Type () const;
    const Data& DataValue () const
    {
        return data_;
    }
    size_t Hash () const;

  private:
    Data data_;
};

// The argument one port hands a node body for one iteration. A scalar port
// (Absent through ArchicadElementRef) carries a single Value, forwarded
// unchanged through Type()/DataValue() so scalar bodies never see this type.
// A ValueType::List port carries a branch instead: Items() is the whole list,
// gathered once per graph run rather than lifted per element. A Value cannot
// nest a List inside itself any more - that shape now lives only here.
class Argument {
  public:
    Argument () = default;
    Argument (Value value) : item_ (std::move (value))
    {
    }

    static Argument FromItems (std::vector<Value> items)
    {
        Argument argument;
        argument.isList_ = true;
        argument.items_ = std::move (items);
        return argument;
    }

    ValueType Type () const
    {
        return isList_ ? ValueType::List : item_.Type ();
    }
    const Value::Data& DataValue () const
    {
        return item_.DataValue ();
    }
    const std::vector<Value>& Items () const
    {
        return items_;
    }

    // The wrapped scalar Value itself, for the few places (tree assembly) that
    // need to hand a Value on rather than read through it. Valid only when
    // Type() != ValueType::List.
    const Value& AsValue () const
    {
        return item_;
    }

    size_t Hash () const;

  private:
    Value item_;
    std::vector<Value> items_;
    bool isList_ = false;
};

} // namespace evp::nodegraph

#endif
