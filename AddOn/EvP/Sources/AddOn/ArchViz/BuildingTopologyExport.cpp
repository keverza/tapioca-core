#include "ArchViz/BuildingTopologyDetail.hpp"
#include "NodeGraph/Json.hpp"

// The complex as one JSON document: everything an offline viewer needs to draw the cells as
// solids and the faces by class, and to answer what a cell touches, without the generator.
namespace geomsrv::archviz::buildingtopology {
namespace {
namespace js = evp::nodegraph::json;
using V = js::JsonValue;

V Number (double value)
{
    return V::Double (std::isfinite (value) ? std::round (value * 1e6) / 1e6 : 0.0);
}
V Id (uint64_t id)
{
    return V::Integer (static_cast<int64_t> (id));
}
V Point (Vec p)
{
    return V::Array ({ Number (p.x), Number (p.y) });
}
V RingOf (const Ring& ring)
{
    js::JsonArray out;
    for (const auto& p : ring)
        out.push_back (Point (p));
    return V::Array (std::move (out));
}
V Rings (const std::vector<Ring>& rings)
{
    js::JsonArray out;
    for (const auto& ring : rings)
        out.push_back (RingOf (ring));
    return V::Array (std::move (out));
}
V Ints (const std::vector<int>& values)
{
    js::JsonArray out;
    for (int v : values)
        out.push_back (V::Integer (v));
    return V::Array (std::move (out));
}
V Ids (const std::vector<uint64_t>& values)
{
    js::JsonArray out;
    for (uint64_t v : values)
        out.push_back (Id (v));
    return V::Array (std::move (out));
}
const char* Name (CellKind kind)
{
    switch (kind) {
        case CellKind::Room:
            return "Room";
        case CellKind::FlatRest:
            return "FlatRest";
        case CellKind::Flat:
            return "Flat";
        case CellKind::Corridor:
            return "Corridor";
        case CellKind::Lobby:
            return "Lobby";
        case CellKind::Core:
            return "Core";
        case CellKind::Unassigned:
            return "Unassigned";
    }
    return "?";
}
const char* Name (floorscheme::RoomKind kind)
{
    switch (kind) {
        case floorscheme::RoomKind::Living:
            return "Living";
        case floorscheme::RoomKind::Bedroom:
            return "Bedroom";
        case floorscheme::RoomKind::Alcove:
            return "Alcove";
        case floorscheme::RoomKind::Hall:
            return "Hall";
        case floorscheme::RoomKind::Bath:
            return "Bath";
        case floorscheme::RoomKind::Wc:
            return "Wc";
        case floorscheme::RoomKind::Storage:
            return "Storage";
    }
    return "?";
}
const char* Name (Beyond beyond)
{
    switch (beyond) {
        case Beyond::Cell:
            return "Cell";
        case Beyond::Outside:
            return "Outside";
        case Beyond::Courtyard:
            return "Courtyard";
        case Beyond::Neighbour:
            return "Neighbour";
        case Beyond::Gap:
            return "Gap";
        case Beyond::Ground:
            return "Ground";
    }
    return "?";
}
const char* Name (FaceClass kind)
{
    switch (kind) {
        case FaceClass::Internal:
            return "Internal";
        case FaceClass::UnitParty:
            return "UnitParty";
        case FaceClass::UnitCorridor:
            return "UnitCorridor";
        case FaceClass::CoreWall:
            return "CoreWall";
        case FaceClass::Open:
            return "Open";
        case FaceClass::Partition:
            return "Partition";
        case FaceClass::Facade:
            return "Facade";
        case FaceClass::BuildingParty:
            return "BuildingParty";
        case FaceClass::Unknown:
            return "Unknown";
        case FaceClass::Slab:
            return "Slab";
        case FaceClass::Shaft:
            return "Shaft";
        case FaceClass::Roof:
            return "Roof";
        case FaceClass::Ground:
            return "Ground";
        case FaceClass::Underside:
            return "Underside";
    }
    return "?";
}
V Links (const Graph& graph)
{
    js::JsonArray out;
    for (const auto& l : graph.links)
        out.push_back (
            V::Array ({ V::Integer (l.from), V::Integer (l.to), V::Integer (l.face), V::Integer (l.aperture) }));
    return V::Array (std::move (out));
}
V Findings (const std::vector<Diagnostic>& found)
{
    js::JsonArray out;
    for (const auto& d : found)
        out.push_back (V::Object ({ { "code", V::String (d.code) },
                                    { "text", V::String (d.text) },
                                    { "floor", V::Integer (d.floor) },
                                    { "at", Point (d.at) } }));
    return V::Array (std::move (out));
}
} // namespace

std::string ToJson (const Complex& complex, size_t indent)
{
    js::JsonArray floors, cells, faces, apertures, units;
    for (size_t i = 0; i < complex.floors.size (); ++i) {
        const auto& f = complex.floors[i];
        floors.push_back (V::Object ({ { "index", V::Integer (static_cast<int64_t> (i)) },
                                       { "story", V::Integer (f.story) },
                                       { "z", Number (f.z) },
                                       { "height", Number (f.height) },
                                       { "outline", Rings (f.outline) },
                                       { "cells", Ints (f.cells) },
                                       { "context", Ints (f.context) },
                                       { "units", Ints (f.units) } }));
    }
    for (size_t i = 0; i < complex.cells.size (); ++i) {
        const auto& c = complex.cells[i];
        cells.push_back (V::Object ({ { "index", V::Integer (static_cast<int64_t> (i)) },
                                      { "id", Id (c.id) },
                                      { "kind", V::String (Name (c.kind)) },
                                      { "room", V::String (c.kind == CellKind::Room ? Name (c.room) : "") },
                                      { "floor", V::Integer (c.floor) },
                                      { "unit", V::Integer (c.unit) },
                                      { "stack", V::Integer (c.stack) },
                                      { "context", V::Bool (c.context) },
                                      { "z0", Number (c.z0) },
                                      { "z1", Number (c.z1) },
                                      { "area", Number (c.area) },
                                      { "volume", Number (Volume (complex, static_cast<int> (i))) },
                                      { "shape", RingOf (c.shape) },
                                      { "faces", Ints (c.faces) },
                                      { "derivedFrom", Ids (c.derivedFrom) } }));
    }
    for (size_t i = 0; i < complex.faces.size (); ++i) {
        const auto& f = complex.faces[i];
        js::JsonObject face { { "index", V::Integer (static_cast<int64_t> (i)) },
                              { "floor", V::Integer (f.floor) },
                              { "horizontal", V::Bool (f.horizontal) },
                              { "kind", V::String (Name (f.kind)) },
                              { "beyond", V::String (Name (f.beyond)) },
                              { "cells", V::Array ({ V::Integer (f.cells[0]), V::Integer (f.cells[1]) }) },
                              { "thickness", Number (f.thickness) },
                              { "area", Number (FaceArea (complex, static_cast<int> (i))) },
                              { "z0", Number (f.z0) },
                              { "z1", Number (f.z1) },
                              { "apertures", Ints (f.apertures) } };
        if (f.horizontal)
            face["region"] = Rings (f.region);
        else
            face["a"] = Point (f.a), face["b"] = Point (f.b);
        faces.push_back (V::Object (std::move (face)));
    }
    for (const auto& a : complex.apertures)
        apertures.push_back (V::Object ({ { "kind", V::String (a.kind == ApertureKind::Door ? "Door" : "Window") },
                                          { "face", V::Integer (a.face) },
                                          { "cell", V::Integer (a.cell) },
                                          { "a", Point (a.a) },
                                          { "b", Point (a.b) } }));
    for (size_t i = 0; i < complex.units.size (); ++i) {
        const auto& u = complex.units[i];
        const auto figures = Figures (complex, static_cast<int> (i));
        units.push_back (
            V::Object ({ { "index", V::Integer (static_cast<int64_t> (i)) },
                         { "id", Id (u.id) },
                         { "floor", V::Integer (u.floor) },
                         { "flat", V::Integer (u.flat) },
                         { "type", V::Integer (static_cast<int64_t> (u.type)) },
                         { "rooms", Number (u.rooms) },
                         { "roomsKnown", V::Bool (u.roomsKnown) },
                         { "cells", Ints (u.cells) },
                         { "derivedFrom", Ids (u.derivedFrom) },
                         { "figures", V::Object ({ { "clearArea", Number (figures.clearArea) },
                                                   { "frontage", Number (figures.frontage) },
                                                   { "partyWall", Number (figures.partyWall) },
                                                   { "windows", V::Integer (figures.windows) },
                                                   { "entrance", V::Bool (figures.entrance) },
                                                   { "reachesStair", V::Bool (figures.reachesStair) } }) } }));
    }
    const V document = V::Object ({ { "format", V::String ("tapioca.cell-complex") },
                                    { "version", V::Integer (1) },
                                    { "units", V::String ("m") },
                                    { "coordinateSystem", V::String ("Archicad project XY and world Z") },
                                    { "floors", V::Array (std::move (floors)) },
                                    { "cells", V::Array (std::move (cells)) },
                                    { "faces", V::Array (std::move (faces)) },
                                    { "apertures", V::Array (std::move (apertures)) },
                                    { "flats", V::Array (std::move (units)) },
                                    { "graphs", V::Object ({ { "adjacency", Links (Adjacency (complex)) },
                                                             { "access", Links (Access (complex)) },
                                                             { "exterior", Links (Exterior (complex)) } }) },
                                    { "check", Findings (Check (complex)) },
                                    { "diagnostics", Findings (complex.diagnostics) } });
    return js::Write (document, indent) + "\n";
}
} // namespace geomsrv::archviz::buildingtopology
