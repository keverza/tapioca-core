#include "ArchViz/DiagramLayout.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>

namespace geomsrv::archviz::diagram {
namespace {
const LevelRule& RuleFor (const LayoutRules& rules, Level level)
{
    static const LevelRule kNone;
    switch (level) {
        case Level::Building:
            return rules.building;
        case Level::Storey:
            return rules.storey;
        case Level::Apartment:
            return rules.apartment;
        case Level::Room:
            return rules.room;
        default:
            return kNone;
    }
}
Representation RepresentationFor (const RepresentationPolicy& policy, Level level)
{
    switch (level) {
        case Level::Building:
        case Level::Parcel:
            return policy.building;
        case Level::Storey:
            return policy.storey;
        case Level::Apartment:
            return policy.apartment;
        default:
            return policy.room;
    }
}
bool Expandable (const Entity& entity)
{
    // An unplanned storey is a shell: it has no apartments to drill into.
    return !(entity.id.level == Level::Storey && !entity.planned);
}
void Grow (Vec3& lo, Vec3& hi, const Vec3& a, const Vec3& b, bool& any)
{
    for (int k = 0; k < 3; ++k) {
        lo[k] = any ? std::min (lo[k], a[k]) : a[k];
        hi[k] = any ? std::max (hi[k], b[k]) : b[k];
    }
    any = true;
}
Vec3 Add (const Vec3& a, const Vec3& b)
{
    return { a[0] + b[0], a[1] + b[1], a[2] + b[2] };
}
std::string Describe (const EntityId& id)
{
    return id.key.empty () ? std::string ("<root>") : id.key;
}
} // namespace

LayoutRules DefaultLayoutRules ()
{
    LayoutRules rules;
    rules.building.strategy = Strategy::Horizontal;
    rules.building.gapFraction = 0.25;
    rules.building.minGapMetres = 2.0;
    rules.building.maxGapMetres = 20.0;
    rules.storey.strategy = Strategy::Vertical;
    return rules;
}

int LayoutPlan::Find (const EntityId& id) const
{
    for (size_t i = 0; i < nodes.size (); ++i)
        if (nodes[i].id == id)
            return int (i);
    return -1;
}

LayoutPlan Plan (const Scene& scene, const LayoutRequest& request)
{
    LayoutPlan plan;
    plan.rules = request.rules;
    std::vector<Entity> entities = scene.entities;
    std::stable_sort (entities.begin (), entities.end (),
                      [] (const Entity& a, const Entity& b) { return a.id < b.id; });
    std::map<EntityId, int> index;
    std::vector<Entity> unique;
    for (const auto& entity : entities) {
        if (entity.id.Empty () || index.count (entity.id)) {
            plan.notes.push_back ("duplicate or empty entity id ignored: " + Describe (entity.id));
            continue;
        }
        index[entity.id] = int (unique.size ());
        unique.push_back (entity);
    }
    std::vector<std::vector<int>> children (unique.size ());
    std::vector<int> roots;
    for (size_t i = 0; i < unique.size (); ++i) {
        const auto& parent = unique[i].parent;
        auto found = parent.Empty () ? index.end () : index.find (parent);
        if (!parent.Empty () && found == index.end ())
            plan.notes.push_back ("unknown parent " + Describe (parent) + " of " + Describe (unique[i].id) +
                                  "; treated as a root");
        if (found == index.end () || found->second == int (i))
            roots.push_back (int (i));
        else
            children[size_t (found->second)].push_back (int (i)); // already in EntityId order
    }

    const std::set<EntityId> expandedSet (request.scope.expanded.begin (), request.scope.expanded.end ());
    const std::set<EntityId> explodeSet (request.scope.explode.begin (), request.scope.explode.end ());
    std::vector<int> nodeOf (unique.size (), -1);
    std::vector<bool> explodes; // per node: its children separate
    // Depth-first from roots in EntityId order: parents precede children, and anything
    // not reachable from a root is a cycle.
    std::vector<std::pair<int, int>> stack; // entity, parent node
    for (auto r = roots.rbegin (); r != roots.rend (); ++r)
        stack.push_back ({ *r, -1 });
    while (!stack.empty ()) {
        auto [e, parentNode] = stack.back ();
        stack.pop_back ();
        const Entity& entity = unique[size_t (e)];
        PlanNode node;
        node.id = entity.id;
        node.parent = parentNode;
        node.depth = parentNode < 0 ? 0 : plan.nodes[size_t (parentNode)].depth + 1;
        node.boundsMin = entity.boundsMin;
        node.boundsMax = entity.boundsMax;
        for (int k = 0; k < 3; ++k)
            node.pivot[k] = 0.5 * (entity.boundsMin[k] + entity.boundsMax[k]);
        node.addressable = entity.addressable;
        node.representation = RepresentationFor (request.representation, entity.id.level);
        const bool parentShows = parentNode < 0 || plan.nodes[size_t (parentNode)].expanded;
        const bool wantsExpand =
            request.scope.expanded.empty () ? entity.id.level < Level::Storey : expandedSet.count (entity.id) > 0;
        node.expanded = parentShows && wantsExpand && Expandable (entity) && !children[size_t (e)].empty ();
        node.visible = parentShows && !node.expanded;
        const bool parentExplodes = parentNode >= 0 && explodes[size_t (parentNode)];
        explodes.push_back (explodeSet.empty () || parentExplodes || explodeSet.count (entity.id) > 0);
        nodeOf[size_t (e)] = int (plan.nodes.size ());
        const int self = int (plan.nodes.size ());
        plan.nodes.push_back (std::move (node));
        const auto& kids = children[size_t (e)];
        for (auto c = kids.rbegin (); c != kids.rend (); ++c)
            stack.push_back ({ *c, self });
    }
    for (size_t i = 0; i < unique.size (); ++i)
        if (nodeOf[i] < 0)
            plan.notes.push_back ("parent cycle: " + Describe (unique[i].id) + " dropped");

    // Linear outward packing per sibling group (same parent, same level). Roots are
    // children of an implicit, always expanded root that explodes when the scope is
    // empty. Never moves inward.
    std::map<std::pair<int, Level>, std::vector<int>> groups;
    for (size_t c = 0; c < plan.nodes.size (); ++c)
        groups[{ plan.nodes[c].parent, plan.nodes[c].id.level }].push_back (int (c));
    for (auto& [group, kids] : groups) {
        const int p = group.first;
        const bool shows = p < 0 || plan.nodes[size_t (p)].expanded;
        const bool separates = p < 0 ? explodeSet.empty () : bool (explodes[size_t (p)]);
        if (!shows || !separates)
            continue;
        const LevelRule& rule = RuleFor (request.rules, group.second);
        if (rule.strategy == Strategy::None)
            continue;
        const int axis = rule.strategy == Strategy::Vertical ? 2 : 0;
        std::stable_sort (kids.begin (), kids.end (), [&] (int a, int b) {
            const auto& na = plan.nodes[size_t (a)];
            const auto& nb = plan.nodes[size_t (b)];
            if (na.boundsMin[axis] != nb.boundsMin[axis])
                return na.boundsMin[axis] < nb.boundsMin[axis];
            return na.id < nb.id;
        });
        double previousEnd = 0, previousExtent = 0;
        for (size_t k = 0; k < kids.size (); ++k) {
            PlanNode& node = plan.nodes[size_t (kids[k])];
            const double start = node.boundsMin[axis];
            const double extent = std::max (0.0, node.boundsMax[axis] - node.boundsMin[axis]);
            double targetStart = start;
            if (k > 0) {
                const double gap = std::clamp (rule.gapFraction * previousExtent, rule.minGapMetres, rule.maxGapMetres);
                targetStart = std::max (start, previousEnd + gap);
            }
            double offset = targetStart - start;
            if (offset > rule.maxOffsetMetres) {
                plan.notes.push_back ("offset of " + Describe (node.id) + " capped at maxOffsetMetres");
                offset = rule.maxOffsetMetres;
            }
            node.target[size_t (axis)] = offset;
            node.sibling = int (k);
            node.siblings = int (kids.size ());
            previousEnd = start + offset + extent;
            previousExtent = extent;
        }
    }

    bool anyA = false, anyE = false;
    std::vector<Vec3> world (plan.nodes.size ());
    for (size_t i = 0; i < plan.nodes.size (); ++i) {
        const auto& node = plan.nodes[i];
        world[i] = node.parent < 0 ? node.target : Add (world[size_t (node.parent)], node.target);
        if (!node.visible)
            continue;
        Grow (plan.assembledMin, plan.assembledMax, node.boundsMin, node.boundsMax, anyA);
        Grow (plan.explodedMin, plan.explodedMax, Add (node.boundsMin, world[i]), Add (node.boundsMax, world[i]), anyE);
    }
    return plan;
}

void FramingBounds (const LayoutPlan& plan, Vec3& lo, Vec3& hi)
{
    for (int k = 0; k < 3; ++k) {
        lo[k] = std::min (plan.assembledMin[k], plan.explodedMin[k]);
        hi[k] = std::max (plan.assembledMax[k], plan.explodedMax[k]);
    }
}

double Ease (Easing easing, double t)
{
    t = std::clamp (t, 0.0, 1.0);
    switch (easing) {
        case Easing::Linear:
            return t;
        case Easing::SmoothStep:
            return t * t * (3.0 - 2.0 * t);
        case Easing::InOutCubic: {
            if (t < 0.5)
                return 4.0 * t * t * t;
            const double u = -2.0 * t + 2.0;
            return 1.0 - u * u * u / 2.0;
        }
    }
    return t;
}

Frame Evaluate (const LayoutPlan& plan, const std::vector<EntityId>& selected, double t)
{
    Frame frame;
    frame.t = std::isfinite (t) ? std::clamp (t, 0.0, 1.0) : 0.0;
    frame.selected = selected;
    std::sort (frame.selected.begin (), frame.selected.end ());
    frame.selected.erase (std::unique (frame.selected.begin (), frame.selected.end ()), frame.selected.end ());
    const double stagger = std::clamp (plan.rules.staggerFraction, 0.0, 0.95);
    frame.poses.reserve (plan.nodes.size ());
    bool any = false;
    for (const auto& node : plan.nodes) {
        NodePose pose;
        pose.id = node.id;
        pose.pivot = node.pivot;
        pose.visible = node.visible;
        pose.representation = node.representation;
        const double delay = node.siblings > 1 ? stagger * double (node.sibling) / double (node.siblings - 1) : 0.0;
        const double local = std::clamp ((frame.t - delay) / (1.0 - stagger), 0.0, 1.0);
        const double e = Ease (plan.rules.easing, local);
        const Vec3 base = node.parent < 0 ? Vec3 {} : frame.poses[size_t (node.parent)].offset;
        for (int k = 0; k < 3; ++k)
            pose.offset[k] = base[k] + e * node.target[k];
        if (pose.visible)
            Grow (frame.boundsMin, frame.boundsMax, Add (node.boundsMin, pose.offset),
                  Add (node.boundsMax, pose.offset), any);
        frame.poses.push_back (std::move (pose));
    }
    return frame;
}

std::array<float, 16> DisplayMatrix (const NodePose& pose, const Vec3& origin)
{
    std::array<float, 16> m {};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    for (int k = 0; k < 3; ++k)
        m[size_t (12 + k)] = float (pose.pivot[k] + pose.offset[k] - origin[k]);
    return m;
}

std::optional<PickHit> Pick (const Frame& frame, const LayoutPlan& plan, const std::vector<PickMesh>& meshes,
                             const Vec3& origin, const Vec3& direction)
{
    const double length =
        std::sqrt (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (!(length > 0) || frame.poses.size () != plan.nodes.size ())
        return std::nullopt;
    const Vec3 d { direction[0] / length, direction[1] / length, direction[2] / length };
    std::map<EntityId, const PickMesh*> byId;
    for (const auto& mesh : meshes)
        byId.emplace (mesh.id, &mesh);
    std::optional<PickHit> best;
    for (size_t i = 0; i < plan.nodes.size (); ++i) {
        const auto& node = plan.nodes[i];
        const auto& pose = frame.poses[i];
        if (!pose.visible || !node.addressable)
            continue;
        auto found = byId.find (node.id);
        if (found == byId.end ())
            continue;
        Vec3 o;
        for (int k = 0; k < 3; ++k)
            o[k] = origin[k] - pose.pivot[k] - pose.offset[k];
        // Coarse prefilter: slab test against the node's local bounds.
        double t0 = 0, t1 = std::numeric_limits<double>::infinity ();
        bool miss = false;
        for (int k = 0; k < 3 && !miss; ++k) {
            const double lo = node.boundsMin[k] - node.pivot[k] - 1e-9;
            const double hi = node.boundsMax[k] - node.pivot[k] + 1e-9;
            if (std::abs (d[k]) < 1e-15) {
                miss = o[k] < lo || o[k] > hi;
                continue;
            }
            double a = (lo - o[k]) / d[k], b = (hi - o[k]) / d[k];
            if (a > b)
                std::swap (a, b);
            t0 = std::max (t0, a);
            t1 = std::min (t1, b);
            miss = t0 > t1;
        }
        if (miss || (best && t0 > best->distance))
            continue;
        const PickMesh& mesh = *found->second;
        for (size_t f = 0; f + 2 < mesh.indices.size (); f += 3) {
            Vec3 v[3];
            bool valid = true;
            for (int c = 0; c < 3; ++c) {
                const size_t at = size_t (mesh.indices[f + size_t (c)]) * 3;
                if (at + 2 >= mesh.positions.size ()) {
                    valid = false;
                    break;
                }
                v[c] = { mesh.positions[at], mesh.positions[at + 1], mesh.positions[at + 2] };
            }
            if (!valid)
                continue;
            // Moller-Trumbore, both faces.
            const Vec3 e1 { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] };
            const Vec3 e2 { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
            const Vec3 p { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
            const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
            if (std::abs (det) < 1e-14)
                continue;
            const double inv = 1.0 / det;
            const Vec3 s { o[0] - v[0][0], o[1] - v[0][1], o[2] - v[0][2] };
            const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
            if (u < 0 || u > 1)
                continue;
            const Vec3 q { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
            const double w = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
            if (w < 0 || u + w > 1)
                continue;
            const double dist = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
            if (dist < 0)
                continue;
            // Ties resolve to the lower EntityId (nodes are visited in that order).
            if (!best || dist < best->distance) {
                PickHit hit;
                hit.id = node.id;
                hit.distance = dist;
                hit.triangle = int (f / 3);
                for (int k = 0; k < 3; ++k)
                    hit.point[k] = origin[k] + d[k] * dist;
                best = hit;
            }
        }
    }
    return best;
}

// ---- Storey identity ----------------------------------------------------------
namespace {
bool MatchesAt (int rank, const StoreySource& a, const StoreySource& b)
{
    const bool sameZ = std::abs (a.z - b.z) <= kStoreyTolerance;
    // Two different persistent IDs are two different storeys, whatever their position:
    // story index and elevation are positional and shift on a mid-stack insert/delete.
    if (rank > 1 && !a.persistentId.empty () && !b.persistentId.empty () && a.persistentId != b.persistentId)
        return false;
    switch (rank) {
        case 1:
            return !a.persistentId.empty () && a.persistentId == b.persistentId;
        case 2:
            return a.sourceStory == b.sourceStory && sameZ;
        case 3:
            return sameZ;
        case 4:
            return a.sourceStory == b.sourceStory;
        case 5:
            return a.displayOrder == b.displayOrder;
        default:
            return false;
    }
}
std::string Label (const StoreySource& s)
{
    char text[96];
    std::snprintf (text, sizeof text, "story %d at z %.2f", s.sourceStory, s.z);
    return text;
}
} // namespace

std::string StoreyPersistentId (int floorId)
{
    return "floor:" + std::to_string (floorId);
}

BindResult BindStoreys (const std::string& buildingKey, const std::vector<StoreyBinding>& previous,
                        const std::vector<StoreySource>& current, uint64_t& nextSerial)
{
    BindResult result;
    result.bindings.resize (current.size ());
    // Canonical visiting order: independent of input vector order.
    std::vector<size_t> order (current.size ());
    for (size_t i = 0; i < order.size (); ++i)
        order[i] = i;
    std::stable_sort (order.begin (), order.end (), [&] (size_t a, size_t b) {
        const auto& x = current[a];
        const auto& y = current[b];
        if (x.z != y.z)
            return x.z < y.z;
        if (x.sourceStory != y.sourceStory)
            return x.sourceStory < y.sourceStory;
        if (x.displayOrder != y.displayOrder)
            return x.displayOrder < y.displayOrder;
        return x.persistentId < y.persistentId;
    });
    std::vector<int> match (current.size (), -1);
    std::vector<bool> refused (current.size (), false), taken (previous.size (), false);
    for (int rank = 1; rank <= 5; ++rank) {
        std::vector<std::vector<size_t>> candidates (current.size ());
        std::vector<int> claims (previous.size (), 0);
        for (size_t i : order) {
            if (match[i] >= 0 || refused[i])
                continue;
            for (size_t j = 0; j < previous.size (); ++j)
                if (!taken[j] && MatchesAt (rank, current[i], previous[j].source))
                    candidates[i].push_back (j);
            if (candidates[i].size () == 1)
                ++claims[candidates[i][0]];
        }
        for (size_t i : order) {
            if (match[i] >= 0 || refused[i] || candidates[i].empty ())
                continue;
            if (candidates[i].size () > 1 || claims[candidates[i][0]] > 1) {
                refused[i] = true;
                result.notes.push_back (Label (current[i]) + ": ambiguous match at rank " + std::to_string (rank) +
                                        "; rebind refused");
                continue;
            }
            match[i] = int (candidates[i][0]);
            taken[candidates[i][0]] = true;
        }
    }
    for (size_t i : order) {
        StoreyBinding binding;
        if (match[i] >= 0) {
            binding = previous[size_t (match[i])];
        }
        else {
            binding.serial = nextSerial++;
            binding.id = { Level::Storey, "storey:" + buildingKey + ":" + std::to_string (binding.serial) };
        }
        binding.source = current[i]; // matching attributes follow the current source
        result.bindings[i] = std::move (binding);
    }
    for (size_t j = 0; j < previous.size (); ++j)
        if (!taken[j])
            result.dropped.push_back (previous[j].id);
    std::sort (result.dropped.begin (), result.dropped.end ());
    return result;
}

EntityId KeepSelection (const EntityId& selected, const BindResult& result, std::vector<std::string>& notes)
{
    if (selected.Empty () || selected.level != Level::Storey)
        return selected;
    for (const auto& binding : result.bindings)
        if (binding.id == selected)
            return selected;
    notes.push_back ("selected " + selected.key + " is no longer bound; selection cleared");
    return {};
}
} // namespace geomsrv::archviz::diagram
