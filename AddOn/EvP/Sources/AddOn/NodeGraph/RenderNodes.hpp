#ifndef EVP_NODEGRAPH_RENDERNODES_HPP
#define EVP_NODEGRAPH_RENDERNODES_HPP

// The render node family: what a headless capture is told, and (from slice 4)
// the capture itself.
//
// ⚠️ RENDERER-FREE, LIKE EVERYTHING ELSE ABOVE IArchicadHost. This file names
// no ArchViz type and includes no Diligent header. What it produces is a STRING
// in the renderer's own vocabulary - the field names StartDiligentCapture's
// schema uses - which is exactly the arrangement the camera codec uses and for
// the same reason: nothing translates between the node and the frame, so there
// is no second place for a field to be spelled differently.
//
// ⚠️ AND IT CARRIES ONLY WHAT THE CAPTURE ACTUALLY ACCEPTS. Sun, environment
// map, exposure, render mode and debug view are GLOBAL VIEWER STATE set by their
// own commands, not capture parameters. Putting them on this node would produce
// a control that looks like it belongs to the capture, outlives the run, and
// makes two settings nodes in one graph fight over the viewer.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/NodeRegistry.hpp"

#include <string>

namespace evp::nodegraph {

// The render-settings node's type id and the output carrying its JSON.
extern const char* const kRenderSettingsNodeType;
extern const char* const kRenderSettingsOutput;

// The capture node: many cameras in, many PNG paths out.
extern const char* const kCaptureNodeType;

// One capture's settings, in StartDiligentCapture's own field names.
//
// ⚠️ THE COLOURS ARE PACKED RGBA HERE AND HEX IN THE NODE. The renderer takes
// one integer per colour; the editor's colour widget is a six-digit hex string
// and carries no alpha. The conversion happens once, in this node's body, so the
// string a downstream capture reads is already what the renderer wants.
struct RenderSettings {
    int64_t width = 1920;
    int64_t height = 1080;
    double dpi = 96.0;
    std::string renderQuality = "realistic";

    bool storySlices = false;
    bool storySliceFill = false;
    std::string storySliceOccluded = "dashed";
    double storySliceWidthPixels = 2.0;
    uint32_t storySliceRgba = 0x3C3C3CFFu;
    uint32_t storySliceFillRgba = 0xC8C8C84Du;
};

std::string EncodeRenderSettings (const RenderSettings& settings);
// False when the text is not a settings object at all. A field that is present
// but wrong is CLAMPED to the schema's range rather than rejected - see the body.
bool DecodeRenderSettings (const std::string& encoded, RenderSettings& settings);

// #RRGGBB plus a 0-255 alpha into the renderer's packed RGBA. An unreadable
// colour yields `fallback`, because a capture that refused to run over a typo in
// a contour colour would be a worse trade than one drawn in the default grey.
uint32_t PackRgba (const std::string& hex, int alpha, uint32_t fallback);

void RegisterRenderNodes (NodeRegistry& registry);
bool IsRenderNodeType (const std::string& nodeTypeId);
bool ExecuteRenderNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                        ValueMap& outputs, std::string& error);

} // namespace evp::nodegraph

#endif
