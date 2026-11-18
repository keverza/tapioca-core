#include "Edit/EditRoof.hpp"
#include "Edit/EditDocument.hpp"
#include <cmath>
#include <set>

namespace geomsrv::edit {
namespace {
std::string Name (const char* what, Id id)
{
    return std::string (what) + " " + std::to_string (id);
}
} // namespace

const RoofNode* FindRoofNode (const RoofDesign& roof, Id node)
{
    for (const auto& n : roof.nodes)
        if (n.id == node)
            return &n;
    return nullptr;
}

RoofNode* FindRoofNode (RoofDesign& roof, Id node)
{
    for (auto& n : roof.nodes)
        if (n.id == node)
            return &n;
    return nullptr;
}

const RoofCell* FindRoofCell (const RoofDesign& roof, Id cell)
{
    for (const auto& c : roof.cells)
        if (c.id == cell)
            return &c;
    return nullptr;
}

bool ValidateRoof (const Document& doc, std::string& error)
{
    if (!doc.roof)
        return true;
    const RoofDesign& roof = *doc.roof;
    std::set<Id> footprints;
    for (Id id : roof.footprints) {
        const Entity* footprint = Find (doc, id);
        if (footprint == nullptr || !footprint->closed || footprint->role != kRoofFootprintRole ||
            !footprints.insert (id).second) {
            error = "every roof footprint must be a distinct closed entity with the roof-footprint role";
            return false;
        }
    }
    if (footprints.empty ()) {
        error = "roof has no footprint";
        return false;
    }
    if (!Finite (roof.origin) || !std::isfinite (roof.bearing) || !std::isfinite (roof.datumZ) ||
        !std::isfinite (roof.ridgeZ) || !std::isfinite (roof.textHeight) || !(roof.textHeight > 0.0)) {
        error = "roof frame, datum and text height must be finite";
        return false;
    }
    if (!(roof.cap > 0.0) || roof.cap > 200.0) {
        error = "roof cell cap must be in (0, 200] m2";
        return false;
    }
    if (!(roof.targetPercent > 0.0) || roof.targetPercent > 100.0) {
        error = "roof target slope must be in (0, 100]%";
        return false;
    }
    if (roof.cells.empty ()) {
        error = "roof has no drainage cells";
        return false;
    }
    for (const auto& n : roof.nodes) {
        if (!Finite (n.p)) {
            error = Name ("roof node", n.id) + " is not finite";
            return false;
        }
        if (n.kind == RoofNodeKind::Drain && n.p.z < roof.datumZ - 1e-6) {
            error = Name ("drain", n.id) + " is below the roof datum";
            return false;
        }
    }
    std::set<Id> used;
    for (const auto& c : roof.cells) {
        std::set<Id> rim;
        for (Id id : c.rim) {
            const RoofNode* n = FindRoofNode (roof, id);
            if (n == nullptr || n->kind != RoofNodeKind::Corner || !rim.insert (id).second) {
                error = Name ("roof cell", c.id) + " has a missing, repeated or non-corner rim node";
                return false;
            }
            used.insert (id);
        }
        if (rim.size () < 3) {
            error = Name ("roof cell", c.id) + " needs at least three rim corners";
            return false;
        }
        const RoofNode* drain = FindRoofNode (roof, c.drain);
        if (drain == nullptr || drain->kind != RoofNodeKind::Drain) {
            error = Name ("roof cell", c.id) + " has no drain";
            return false;
        }
        if (!used.insert (c.drain).second) {
            error = Name ("drain", c.drain) + " belongs to more than one cell";
            return false;
        }
    }
    for (const auto& n : roof.nodes)
        if (!used.count (n.id)) {
            error = Name ("roof node", n.id) + " belongs to no cell";
            return false;
        }
    return true;
}

} // namespace geomsrv::edit
