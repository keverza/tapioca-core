#pragma once
#include "NativePreview.hpp"

namespace tapioca::playground {
Value WorkflowState (MockServices& services);
bool RestoreWorkflowState (MockServices& services, const Value& state);
// Replaces the editor session with a "hudeditor" fixture's dump: its document through the editor's
// own dump reader, then its `playground` selection and session (tool, command and its picks, the
// close prompt). False, with the reason, when the dump does not read; the session is then kept.
bool LoadEditorFixture (MockServices& services, const Value& dump, std::string& error);
} // namespace tapioca::playground
