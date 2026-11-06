#include "Edit/EditBoolean.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditShape.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace geomsrv::edit {

namespace {

namespace cp = Clipper2Lib;
constexpr int kPrecision = 6;            // 1e-6 m, the editor's kMinSegment
constexpr size_t kMaxPoints = 200000;    // flattened input points over all operands
constexpr double kMaxCoordinate = 1.0e9; // metres; Clipper's integer range at precision 6

cp::PathsD ToPaths (const std::vector<std::vector<Vec3>>& rings)
{
    cp::PathsD paths;
    for (const auto& ring : rings) {
        cp::PathD path;
        for (const auto& p : ring)
            path.push_back ({ p.x, p.y });
        paths.push_back (std::move (path));
    }
    return paths;
}

std::vector<Vec3> FromPath (const cp::PathD& path, double z)
{
    std::vector<Vec3> ring;
    for (const auto& p : path)
        ring.push_back ({ p.x, p.y, z });
    return ring;
}

// A top-level outline and everything nested in it, at any depth.
void Collect (const cp::PolyPathD& node, double z, BooleanPiece& piece)
{
    if (!node.Polygon ().empty ())
        piece.push_back (FromPath (node.Polygon (), z));
    for (const auto& child : node)
        Collect (*child, z, piece);
}

// The area inside a piece's outline, holes ignored: what ranks the pieces.
double OutlineArea (const BooleanPiece& piece)
{
    const auto& ring = piece.front ();
    double twice = 0.0;
    for (size_t i = 0; i < ring.size (); ++i) {
        const Vec3 a = ring[i], b = ring[(i + 1) % ring.size ()];
        twice += a.x * b.y - b.x * a.y;
    }
    return std::abs (twice) * 0.5;
}

bool LandscapeOwned (const Document& doc, Id entity)
{
    if (!doc.landscape)
        return false;
    for (const auto& source : doc.landscape->sources)
        if (!source.deleted && source.entityId == entity)
            return true;
    return false;
}

// Every anchor's Z, for a polyline; the base for an extrusion (Validate keeps it flat).
bool Flat (const Entity& e, double z)
{
    for (const auto& v : e.vertices)
        if (std::abs (v.p.z - z) > kBooleanPlaneTolerance)
            return false;
    return true;
}

} // namespace

const char* BooleanName (BooleanKind kind)
{
    switch (kind) {
        case BooleanKind::Union:
            return "union";
        case BooleanKind::Subtract:
            return "subtract";
        case BooleanKind::Intersect:
            return "intersect";
        case BooleanKind::Exclude:
            return "exclude";
    }
    return "boolean";
}

bool BooleanRings (const std::vector<std::vector<std::vector<Vec3>>>& operands, BooleanKind kind, double z,
                   std::vector<BooleanPiece>& pieces, std::string& error)
{
    pieces.clear ();
    if (operands.size () < 2) {
        error = "a boolean needs at least two operands";
        return false;
    }
    size_t points = 0;
    for (const auto& operand : operands)
        for (const auto& ring : operand)
            for (const auto& p : ring) {
                if (!std::isfinite (p.x) || !std::isfinite (p.y) || std::abs (p.x) > kMaxCoordinate ||
                    std::abs (p.y) > kMaxCoordinate) {
                    error = "boolean coordinates must be finite and within +/-1e9 m";
                    return false;
                }
                ++points;
            }
    if (points > kMaxPoints) {
        error = "the operands have more than 200000 boundary points together";
        return false;
    }
    // Each operand's own even-odd fill first, as non-overlapping positive outlines and holes, so
    // the operands below meet under non-zero fill and never cancel each other by parity.
    std::vector<cp::PathsD> solid;
    for (const auto& operand : operands)
        solid.push_back (cp::Union (ToPaths (operand), cp::FillRule::EvenOdd, kPrecision));
    cp::PathsD subject = solid.front (), clip;
    cp::ClipType type = cp::ClipType::Union;
    switch (kind) {
        case BooleanKind::Union:
            for (size_t i = 1; i < solid.size (); ++i)
                subject.insert (subject.end (), solid[i].begin (), solid[i].end ());
            break;
        case BooleanKind::Subtract:
            for (size_t i = 1; i < solid.size (); ++i)
                clip.insert (clip.end (), solid[i].begin (), solid[i].end ());
            type = cp::ClipType::Difference;
            break;
        case BooleanKind::Intersect:
            for (size_t i = 1; i + 1 < solid.size (); ++i)
                subject = cp::Intersect (subject, solid[i], cp::FillRule::NonZero, kPrecision);
            clip = solid.back ();
            type = cp::ClipType::Intersection;
            break;
        case BooleanKind::Exclude:
            for (size_t i = 1; i + 1 < solid.size (); ++i)
                subject = cp::Xor (subject, solid[i], cp::FillRule::NonZero, kPrecision);
            clip = solid.back ();
            type = cp::ClipType::Xor;
            break;
    }
    cp::PolyTreeD tree;
    cp::BooleanOp (type, cp::FillRule::NonZero, subject, clip, tree, kPrecision);
    for (const auto& outer : tree) {
        BooleanPiece piece;
        Collect (*outer, z, piece);
        if (!piece.empty () && piece.front ().size () >= 3)
            pieces.push_back (std::move (piece));
    }
    std::stable_sort (pieces.begin (), pieces.end (),
                      [] (const BooleanPiece& a, const BooleanPiece& b) { return OutlineArea (a) > OutlineArea (b); });
    return true;
}

bool BooleanEntities (Document& doc, const std::vector<Id>& operands, BooleanKind kind, std::string& error,
                      std::vector<Id>* results)
{
    if (operands.size () < 2) {
        error = "select at least two closed outlines";
        return false;
    }
    if (std::set<Id> (operands.begin (), operands.end ()).size () != operands.size ()) {
        error = "an outline is selected twice";
        return false;
    }
    const Entity* first = Find (doc, operands.front ());
    std::vector<std::vector<std::vector<Vec3>>> rings;
    double z = 0.0;
    for (const Id id : operands) {
        const Entity* e = Find (doc, id);
        if (e == nullptr || e->vertices.empty ()) {
            error = "an operand no longer exists";
            return false;
        }
        if (!e->closed) {
            error = "booleans need closed outlines; close the open line first";
            return false;
        }
        if (!Pickable (doc, e->member)) {
            error = "a hidden or locked outline cannot take part in a boolean";
            return false;
        }
        if (LandscapeOwned (doc, id)) {
            error = "that outline is a landscape source; combine landscape regions in the landscape panel";
            return false;
        }
        if (e->kind != first->kind) {
            error = "booleans combine outlines with outlines, or solids with solids";
            return false;
        }
        if (id == operands.front ())
            z = e->vertices.front ().p.z;
        if (!Flat (*e, z)) {
            error = "booleans need every operand flat and in one horizontal plane";
            return false;
        }
        rings.push_back (ShapeRings (*e, kBooleanMaxAngle));
    }
    std::vector<BooleanPiece> pieces;
    if (!BooleanRings (rings, kind, z, pieces, error))
        return false;
    if (pieces.empty ()) {
        error = std::string ("the ") + BooleanName (kind) + " is empty: nothing would be left";
        return false;
    }
    // Built on a copy, swapped in only once every piece is a valid entity.
    Document candidate = doc;
    const Entity keep = *first;
    if (!SetRings (candidate, keep.id, pieces.front (), error)) {
        error = std::string ("the ") + BooleanName (kind) + " gives an outline the editor cannot hold: " + error;
        return false;
    }
    std::vector<Id> made { keep.id };
    for (size_t i = 1; i < pieces.size (); ++i) {
        const Id id = AddCompound (candidate, pieces[i], keep.kind, keep.height, keep.role);
        if (id == kNoId) {
            error = std::string ("the ") + BooleanName (kind) +
                    " gives a piece whose contours touch; move the operands apart or together first";
            return false;
        }
        Find (candidate, id)->member = keep.member;
        made.push_back (id);
    }
    const std::set<Id> gone (operands.begin () + 1, operands.end ());
    candidate.entities.erase (std::remove_if (candidate.entities.begin (), candidate.entities.end (),
                                              [&] (const Entity& e) { return gone.count (e.id) != 0; }),
                              candidate.entities.end ());
    candidate.isolated.erase (std::remove_if (candidate.isolated.begin (), candidate.isolated.end (),
                                              [&] (Id id) { return gone.count (id) != 0; }),
                              candidate.isolated.end ());
    if (!Validate (candidate, error))
        return false;
    candidate.revision = doc.revision + 1;
    doc = std::move (candidate);
    if (results != nullptr)
        *results = std::move (made);
    return true;
}

} // namespace geomsrv::edit
