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
    using List = std::vector<Value>;
    using Data = std::variant<std::monostate, bool, int64_t, double, std::string, Point3, Polyline, Polygon,
                              ImmutableMesh, ArchicadElementRef, List>;

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
    explicit Value (List value) : data_ (std::move (value))
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

} // namespace evp::nodegraph

#endif
