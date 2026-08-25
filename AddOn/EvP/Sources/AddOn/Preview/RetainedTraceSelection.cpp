#include "Annotation/RetainedTraceSelection.hpp"

#include "Preview/RetainedTraceSelection.hpp"

#include <mutex>

namespace {

geomsrv::annotation::PrimitiveKind KindOf (evp::preview::WatchPrimitiveKind kind)
{
    using A = geomsrv::annotation::PrimitiveKind;
    switch (kind) {
        case evp::preview::WatchPrimitiveKind::Point:
            return A::Point;
        case evp::preview::WatchPrimitiveKind::Polyline:
            return A::Polyline;
        case evp::preview::WatchPrimitiveKind::Arrow:
            return A::Arrow;
        case evp::preview::WatchPrimitiveKind::Dimension:
            return A::Dimension;
        case evp::preview::WatchPrimitiveKind::Angle:
            return A::Angle;
        case evp::preview::WatchPrimitiveKind::Label:
            return A::Label;
        case evp::preview::WatchPrimitiveKind::Element:
            return A::Element;
    }
    return A::Point;
}

geomsrv::annotation::SemanticRole RoleOf (const std::optional<std::string>& role)
{
    using R = geomsrv::annotation::SemanticRole;
    if (role == "add")
        return R::Add;
    if (role == "remove")
        return R::Remove;
    if (role == "modify")
        return R::Modify;
    if (role == "context")
        return R::Context;
    if (role == "guide")
        return R::Guide;
    return R::None;
}

} // namespace

namespace evp::preview {

geomsrv::annotation::DrawList ToDrawList (const WatchTrace& trace)
{
    geomsrv::annotation::DrawList result;
    result.nodes.reserve (trace.nodes.size ());
    for (const WatchNode& sourceNode : trace.nodes) {
        geomsrv::annotation::Node node;
        node.name = sourceNode.name;
        node.frames.reserve (sourceNode.frames.size ());
        for (const WatchFrame& sourceFrame : sourceNode.frames) {
            geomsrv::annotation::Frame frame;
            frame.index = sourceFrame.index;
            frame.primitives.reserve (sourceFrame.primitives.size ());
            for (const WatchPrimitive& source : sourceFrame.primitives) {
                geomsrv::annotation::Primitive primitive;
                primitive.kind = KindOf (source.kind);
                primitive.role = RoleOf (source.role);
                primitive.guid = source.guid;
                primitive.text = source.text.value_or ("");
                primitive.closed = source.closed.value_or (false);
                primitive.direction = source.direction.value_or (false);
                primitive.offset = source.offset.value_or (0.0);
                primitive.points.reserve (source.points.size () / 3);
                for (std::size_t index = 0; index + 2 < source.points.size (); index += 3)
                    primitive.points.push_back (
                        { source.points[index], source.points[index + 1], source.points[index + 2] });
                frame.primitives.push_back (std::move (primitive));
            }
            node.frames.push_back (std::move (frame));
        }
        result.nodes.push_back (std::move (node));
    }
    return result;
}

} // namespace evp::preview

namespace geomsrv::annotation {

std::optional<RetainedFrameSnapshot> SelectedRetainedFrameSnapshotCopy ()
{
    const auto selected = evp::preview::RetainedPreviewStore::Get ().SelectedWatchFrameSnapshotCopy ();
    if (!selected.has_value ())
        return std::nullopt;

    static std::mutex cacheMutex;
    static uint64_t cachedGeneration = 0;
    static std::shared_ptr<const DrawList> cachedDrawList;
    std::lock_guard<std::mutex> lock (cacheMutex);
    if (cachedGeneration != selected->snapshot->generation) {
        cachedDrawList = std::make_shared<const DrawList> (evp::preview::ToDrawList (selected->snapshot->trace));
        cachedGeneration = selected->snapshot->generation;
    }
    return RetainedFrameSnapshot { cachedDrawList, selected->nodeIndex, selected->frameIndex };
}

} // namespace geomsrv::annotation
