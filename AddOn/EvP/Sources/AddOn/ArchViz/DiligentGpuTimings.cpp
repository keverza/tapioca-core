#include "ArchViz/DiligentGpuTimings.hpp"

#include "ArchViz/ArchVizLog.hpp"

#include <DeviceContext.h>
#include <Query.h>
#include <RefCntAutoPtr.hpp>
#include <RenderDevice.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace geomsrv::archviz {

namespace {

constexpr size_t kFrameCount = 8;
constexpr size_t kStageCount = 3;

size_t StageIndex (GpuTimingStage stage)
{
    switch (stage) {
        case GpuTimingStage::VisibilityGBuffer:
            return 0;
        case GpuTimingStage::Shading:
            return 1;
        case GpuTimingStage::Post:
            return 2;
    }
    return kStageCount;
}

struct QueryPair {
    Diligent::RefCntAutoPtr<Diligent::IQuery> begin;
    Diligent::RefCntAutoPtr<Diligent::IQuery> end;
};

struct FrameQueries {
    uint64_t frame = 0;
    uint32_t sampleCount = 0;
    bool submitted = false;
    bool reported = false;
    std::array<bool, kStageCount> started = {};
    std::array<bool, kStageCount> ended = {};
    std::array<QueryPair, kStageCount> stages;

    void Reset (uint64_t frameNumber)
    {
        frame = frameNumber;
        sampleCount = 0;
        submitted = false;
        reported = false;
        started.fill (false);
        ended.fill (false);
    }
};

} // namespace

struct DiligentGpuTimings::Impl {
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;
    std::array<FrameQueries, kFrameCount> frames;
    FrameQueries* current = nullptr;
    bool enabled = false;
};

DiligentGpuTimings::DiligentGpuTimings () : impl_ (new Impl {})
{
}

DiligentGpuTimings::~DiligentGpuTimings ()
{
    Shutdown ();
    delete impl_;
}

bool DiligentGpuTimings::Initialize (Diligent::IRenderDevice* device)
{
    Shutdown ();
    const char* benchmark = std::getenv ("TAPIOCA_ARCHVIZ_BENCHMARK");
    if (benchmark == nullptr || benchmark[0] != '1' || device == nullptr ||
        device->GetDeviceInfo ().Features.TimestampQueries != Diligent::DEVICE_FEATURE_STATE_ENABLED)
        return false;

    impl_->device = device;
    Diligent::QueryDesc queryDesc { Diligent::QUERY_TYPE_TIMESTAMP };
    for (FrameQueries& frame : impl_->frames) {
        for (QueryPair& stage : frame.stages) {
            queryDesc.Name = "Tapioca ArchViz GPU timing begin";
            device->CreateQuery (queryDesc, &stage.begin);
            queryDesc.Name = "Tapioca ArchViz GPU timing end";
            device->CreateQuery (queryDesc, &stage.end);
            if (stage.begin == nullptr || stage.end == nullptr) {
                Shutdown ();
                return false;
            }
        }
    }
    impl_->enabled = true;
    return true;
}

void DiligentGpuTimings::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    impl_->enabled = false;
    impl_->current = nullptr;
    for (FrameQueries& frame : impl_->frames) {
        frame.submitted = false;
        frame.reported = false;
        for (QueryPair& stage : frame.stages) {
            stage.begin.Release ();
            stage.end.Release ();
        }
    }
    impl_->device.Release ();
}

bool DiligentGpuTimings::Enabled () const
{
    return impl_ != nullptr && impl_->enabled;
}

void DiligentGpuTimings::BeginFrame (uint64_t frame)
{
    if (!Enabled ())
        return;

    Collect ();
    FrameQueries& slot = impl_->frames[frame % kFrameCount];
    // A healthy D3D11 queue completes these well before the ring wraps. If it
    // does not, skip instrumentation for this frame rather than blocking the
    // render thread waiting for a driver query.
    if (slot.submitted && !slot.reported) {
        impl_->current = nullptr;
        return;
    }
    slot.Reset (frame);
    impl_->current = &slot;
}

void DiligentGpuTimings::Begin (Diligent::IDeviceContext* context, GpuTimingStage stage)
{
    if (!Enabled () || impl_->current == nullptr || context == nullptr)
        return;
    const size_t index = StageIndex (stage);
    if (index >= kStageCount || impl_->current->started[index])
        return;
    context->EndQuery (impl_->current->stages[index].begin);
    impl_->current->started[index] = true;
}

void DiligentGpuTimings::End (Diligent::IDeviceContext* context, GpuTimingStage stage)
{
    if (!Enabled () || impl_->current == nullptr || context == nullptr)
        return;
    const size_t index = StageIndex (stage);
    if (index >= kStageCount || !impl_->current->started[index] || impl_->current->ended[index])
        return;
    context->EndQuery (impl_->current->stages[index].end);
    impl_->current->ended[index] = true;
}

void DiligentGpuTimings::EndFrame (uint64_t frame, uint32_t sampleCount)
{
    if (!Enabled () || impl_->current == nullptr || impl_->current->frame != frame)
        return;
    for (size_t index = 0; index < kStageCount; ++index) {
        if (!impl_->current->ended[index]) {
            impl_->current = nullptr;
            return;
        }
    }
    impl_->current->sampleCount = sampleCount;
    impl_->current->submitted = true;
    impl_->current = nullptr;
}

void DiligentGpuTimings::Collect ()
{
    if (!Enabled ())
        return;

    for (FrameQueries& frame : impl_->frames) {
        if (!frame.submitted || frame.reported)
            continue;

        std::array<double, kStageCount> durations = {};
        bool available = true;
        for (size_t index = 0; index < kStageCount; ++index) {
            Diligent::QueryDataTimestamp start;
            Diligent::QueryDataTimestamp end;
            const QueryPair& stage = frame.stages[index];
            if (!stage.begin->GetData (&start, sizeof (start), false) ||
                !stage.end->GetData (&end, sizeof (end), false) || start.Frequency == 0 || end.Frequency == 0 ||
                end.Counter < start.Counter) {
                available = false;
                break;
            }
            durations[index] = (static_cast<double> (end.Counter) / static_cast<double> (end.Frequency) -
                                static_cast<double> (start.Counter) / static_cast<double> (start.Frequency)) *
                               1000.0;
        }
        if (!available)
            continue;

        ArchVizLog ("Diligent benchmark frame=" + std::to_string (frame.frame) +
                    " visibility_gbuffer_ms=" + std::to_string (durations[0]) +
                    " shading_ms=" + std::to_string (durations[1]) + " post_ms=" + std::to_string (durations[2]) +
                    " sample_count=" + std::to_string (frame.sampleCount));
        frame.reported = true;
        for (QueryPair& stage : frame.stages) {
            stage.begin->Invalidate ();
            stage.end->Invalidate ();
        }
    }
}

} // namespace geomsrv::archviz
