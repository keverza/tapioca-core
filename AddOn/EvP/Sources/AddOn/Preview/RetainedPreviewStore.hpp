#ifndef EVP_PREVIEW_RETAINEDPREVIEWSTORE_HPP
#define EVP_PREVIEW_RETAINEDPREVIEWSTORE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace evp::preview {

using Point3 = std::array<double, 3>;

struct PreviewMesh {
    std::string role;
    std::string label;
    std::vector<double> vertices;
    std::vector<double> normals;
    std::vector<uint32_t> triangles;
};

struct PreviewLine {
    std::string role;
    std::string label;
    bool closed = false;
    std::vector<double> points;
};

struct PreviewScene {
    std::string kind;
    std::vector<PreviewMesh> meshes;
    std::vector<PreviewLine> lines;
    std::vector<std::string> notes;
    bool hasBounds = false;
    Point3 boundsMin {};
    Point3 boundsMax {};
};

enum class WatchPrimitiveKind { Point, Polyline, Arrow, Dimension, Angle, Label, Element };

struct WatchPrimitive {
    WatchPrimitiveKind kind = WatchPrimitiveKind::Point;
    std::vector<double> points;
    std::string guid;
    std::optional<std::string> text;
    std::optional<std::string> role;
    std::optional<bool> closed;
    std::optional<bool> direction;
    std::optional<double> offset;
};

struct WatchFrame {
    uint32_t index = 0;
    std::vector<WatchPrimitive> primitives;
};

struct WatchNode {
    std::string name;
    std::vector<WatchFrame> frames;
};

struct WatchTrace {
    uint32_t version = 1;
    std::vector<WatchNode> nodes;
};

struct PreviewSceneSnapshot {
    uint64_t generation = 0;
    PreviewScene scene;
};

struct WatchTraceSnapshot {
    uint64_t generation = 0;
    WatchTrace trace;
};

struct SelectedWatchFrameSnapshot {
    std::shared_ptr<const WatchTraceSnapshot> snapshot;
    std::size_t nodeIndex = 0;
    std::size_t frameIndex = 0;

    const WatchNode& Node () const;
    const WatchFrame& Frame () const;
};

// Any thread may publish or retain a snapshot. Published objects never mutate,
// so render hosts can keep a shared pointer without holding the store lock.
class RetainedPreviewStore {
  public:
    static RetainedPreviewStore& Get ();

    uint64_t PublishPreviewScene (PreviewScene scene);
    uint64_t PublishWatchTrace (WatchTrace trace);

    std::shared_ptr<const PreviewSceneSnapshot> PreviewSceneSnapshotCopy () const;
    std::shared_ptr<const WatchTraceSnapshot> WatchTraceSnapshotCopy () const;
    std::optional<SelectedWatchFrameSnapshot> SelectedWatchFrameSnapshotCopy () const;

    bool SelectWatchFrame (std::size_t nodeIndex, std::size_t frameIndex);
    void ClearWatchSelection ();

  private:
    mutable std::mutex mutex;
    uint64_t generation = 0;
    std::shared_ptr<const PreviewSceneSnapshot> previewScene;
    std::shared_ptr<const WatchTraceSnapshot> watchTrace;
    std::optional<std::size_t> selectedWatchNode;
    std::optional<std::size_t> selectedWatchFrame;
};

} // namespace evp::preview

#endif
