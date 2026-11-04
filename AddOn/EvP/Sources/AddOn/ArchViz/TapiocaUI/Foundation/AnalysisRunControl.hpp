#ifndef TAPIOCA_UI_ANALYSIS_RUN_CONTROL_HPP
#define TAPIOCA_UI_ANALYSIS_RUN_CONTROL_HPP

#include <imgui.h>
#include <cstddef>

namespace tapioca::ui {

enum class AnalysisRunStage { Idle, Waiting, Capturing, Preparing, Calculating, Cancelling, Publishing, Failed };
enum class AnalysisRunAction { None, Run, Cancel };

struct AnalysisRunView {
    AnalysisRunStage stage = AnalysisRunStage::Idle;
    const char* runLabel = "Run analysis";
    size_t resolved = 0, total = 0;
    bool enabled = true;
    bool cancellable = true; // false after an atomic publication has begun
};

// Presentation only: one stable, keyboard-navigable hit area. No worker, SDK,
// cancellation token or retained job state is owned by the widget.
AnalysisRunAction AnalysisRunControl (const char* id, const AnalysisRunView& view);

} // namespace tapioca::ui
#endif
