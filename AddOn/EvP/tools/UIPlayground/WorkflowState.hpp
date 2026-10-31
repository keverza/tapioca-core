#pragma once
#include "NativePreview.hpp"

namespace tapioca::playground {
Value WorkflowState (MockServices& services);
bool RestoreWorkflowState (MockServices& services, const Value& state);
} // namespace tapioca::playground
