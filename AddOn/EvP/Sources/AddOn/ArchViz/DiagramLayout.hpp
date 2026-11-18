#ifndef EVP_ARCHVIZ_DIAGRAMLAYOUT_HPP
#define EVP_ARCHVIZ_DIAGRAMLAYOUT_HPP
// Exploded building diagrams: a presentation-only layout over the massing graph.
//
// Pure and read-only: no ACAPI, ImGui, renderer or model writes. Plan is camera-
// independent and runs when the scene or request changes; Evaluate derives every
// pose from source at a given t and never reads a previous frame. Every sibling list
// is ordered by a documented stable key, so the same inputs give the same frame
// regardless of input vector order. Offsets are double; only DisplayMatrix rounds to
// float, relative to a caller-chosen render origin (survey coordinates stay exact).
#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace geomsrv::archviz::diagram {
using Vec3 = std::array<double, 3>;

enum class Level : uint8_t { Parcel, Building, Storey, Apartment, Room, Space };

struct EntityId {
    Level level = Level::Building;
    std::string key; // semantic: "building:<id>", "storey:<buildingKey>:<serial>", ...
    bool Empty () const
    {
        return key.empty ();
    }
    auto operator<=> (const EntityId&) const = default;
};

struct SourceRef {
    std::string parcelGuid;  // property line, empty when unassigned
    std::string buildingKey; // massingbuildings::Group.key
    std::string sourceGuid;  // source element, when one exists
    int sourceStory = 0;
    uint64_t unitId = 0, cellId = 0;
};

struct Entity {
    EntityId id;
    EntityId parent; // empty for a root
    SourceRef source;
    Vec3 boundsMin {}, boundsMax {}; // world metres at the source pose
    bool planned = false;            // false: a storey shell with no apartment/room children
    bool addressable = true;         // false: aggregate without its own pick/label target
};

struct Scene {
    std::vector<Entity> entities; // any order; Plan sorts by EntityId
    std::vector<std::string> notes;
};

enum class Strategy : uint8_t { None, Horizontal, Vertical };
enum class Easing : uint8_t { Linear, SmoothStep, InOutCubic };
enum class Representation : uint8_t { Shell, CellVolume, Detail };

struct LevelRule {
    Strategy strategy = Strategy::None;
    double gapFraction = 0.5; // of the previous sibling's extent on the axis
    double minGapMetres = 0.6;
    double maxGapMetres = 6.0;
    double maxOffsetMetres = 200.0; // hard cap; exceeding it is reported, never grown
};

struct LayoutRules {
    LevelRule building, storey, apartment, room;
    Easing easing = Easing::SmoothStep;
    double staggerFraction = 0.0; // total normalised delay across siblings, in [0, 1)
};
// Storeys stack along +Z by one storey height (1..8 m); buildings stay put (sideways
// separation is deferred); apartments/rooms stay (M2).
LayoutRules DefaultLayoutRules ();

struct RepresentationPolicy {
    Representation building = Representation::Shell;
    Representation storey = Representation::Shell;
    Representation apartment = Representation::CellVolume;
    Representation room = Representation::CellVolume;
};

struct LayoutScope {
    std::vector<EntityId> explode;  // parents whose subtrees separate; empty = all
    std::vector<EntityId> expanded; // parents whose children are shown; empty = down to storeys
};

struct LayoutRequest {
    LayoutRules rules;
    LayoutScope scope;
    RepresentationPolicy representation;
};

struct PlanNode {
    EntityId id;
    int parent = -1; // index into LayoutPlan::nodes; parents precede children
    int depth = 0;
    Vec3 boundsMin {}, boundsMax {};
    Vec3 pivot {};   // source bounds centre; geometry is stored relative to it
    Vec3 target {};  // offset in parent axes at t = 1
    int sibling = 0; // rank among moving siblings, for stagger
    int siblings = 1;
    bool stacked = false; // moved by a Vertical rule: Spaced lifts it by `sibling` gaps
    bool visible = false; // exactly one owner per region: no visible ancestor or descendant
    bool expanded = false;
    bool addressable = true;
    Representation representation = Representation::Shell;
};

struct LayoutPlan {
    std::vector<PlanNode> nodes; // parent before child, then by EntityId
    LayoutRules rules;
    Vec3 assembledMin {}, assembledMax {}; // t = 0
    Vec3 explodedMin {}, explodedMax {};   // t = 1
    std::vector<std::string> notes;        // caps, cycles, unknown parents
    int Find (const EntityId& id) const;
};

LayoutPlan Plan (const Scene& scene, const LayoutRequest& request);

// Union of assembled and exploded bounds, so animation never leaves the frame.
void FramingBounds (const LayoutPlan& plan, Vec3& lo, Vec3& hi);

// ---- Spacing: the explode as a distance (#201, user 2026-11-18) ------------------
// "Exploding vertical should be not percent but offset distance", and a building can
// carry its own. A stacked storey of rank k lifts by k gaps of its building: linear in
// the gap, so the same plan serves every distance and a slider never rebuilds.
constexpr double kMaxStoreyGap = 200.0; // m; a gap is clamped to [0, this]
struct Spacing {
    double storeyGap = 3.0;                    // m between consecutive storeys
    std::map<std::string, double> buildingGap; // building EntityId key -> its own gap
    double GapOf (const std::string& buildingKey) const;
    auto operator<=> (const Spacing&) const = default;
};
// The plan with every stacked node's target replaced by rank x its building's gap, and
// the exploded bounds recomputed. Nodes the rules did not stack keep their targets.
LayoutPlan Spaced (const LayoutPlan& plan, const Spacing& spacing);
// One animation step from `shown` toward `target`: every gap moves by `fraction` of the
// way. An override that is gone eases back to the shared gap before it is dropped.
Spacing Approach (const Spacing& shown, const Spacing& target, double fraction);

struct NodePose {
    EntityId id;
    Vec3 pivot {};  // source pivot, world
    Vec3 offset {}; // world displacement at t, composed from every ancestor
    bool visible = false;
    Representation representation = Representation::Shell;
};

struct Frame {
    double t = 0;
    std::vector<NodePose> poses; // same order as LayoutPlan::nodes
    std::vector<EntityId> selected;
    Vec3 boundsMin {}, boundsMax {}; // visible nodes at t
};

double Ease (Easing easing, double t);
Frame Evaluate (const LayoutPlan& plan, const std::vector<EntityId>& selected, double t);

// Column-major float matrix placing node-local geometry (relative to its pivot)
// at its display pose, expressed relative to `origin`.
std::array<float, 16> DisplayMatrix (const NodePose& pose, const Vec3& origin);

// Node-local triangles: positions relative to the node's pivot.
struct PickMesh {
    EntityId id;
    std::vector<double> positions; // xyz
    std::vector<uint32_t> indices;
};
struct PickHit {
    EntityId id;
    Vec3 point {};
    double distance = 0;
    int triangle = -1;
};
// Exact: transformed bounds prefilter, then ray-triangle against visible nodes only.
std::optional<PickHit> Pick (const Frame& frame, const LayoutPlan& plan, const std::vector<PickMesh>& meshes,
                             const Vec3& origin, const Vec3& direction);

// ---- Storey identity --------------------------------------------------------
// Elevation and order are match attributes, never identity. A binding's serial is
// session-stable. persistentId is the Archicad story's floorId (StoreyPersistentId),
// which survives a mid-stack insert/delete; without it the ranks are positional and
// such an edit can move identity (#222).
constexpr double kStoreyTolerance = 0.05; // m, = buildingplan::kElevationTolerance

std::string StoreyPersistentId (int floorId); // "floor:<id>"

struct StoreySource {
    std::string persistentId;
    int sourceStory = 0;
    double z = 0;
    int displayOrder = 0;
};
struct StoreyBinding {
    EntityId id; // storey:<buildingKey>:<serial>
    uint64_t serial = 0;
    StoreySource source;
};
struct BindResult {
    std::vector<StoreyBinding> bindings; // aligned with `current`
    std::vector<EntityId> dropped;       // previous bindings with no successor
    std::vector<std::string> notes;      // ambiguous matches, refused rebinds
};
// Canonical order: persistent ID -> story and elevation -> unique elevation ->
// unique story -> unique display order -> new serial. Two candidates at one rank
// refuse the rebind (new serial and a note); a value never becomes the identity.
// Storeys whose persistent IDs are both set and differ never match positionally.
BindResult BindStoreys (const std::string& buildingKey, const std::vector<StoreyBinding>& previous,
                        const std::vector<StoreySource>& current, uint64_t& nextSerial);
// The selection survives only when its binding does; otherwise empty plus a note.
EntityId KeepSelection (const EntityId& selected, const BindResult& result, std::vector<std::string>& notes);
} // namespace geomsrv::archviz::diagram
#endif
