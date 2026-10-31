#pragma once
#include "Fixture.hpp"
#include "UiAbi.h"
#include "ArchViz/TapiocaUI/Demo/WidgetGallery.hpp"
#include "ArchViz/HudFloorProgramme.hpp"
#include "NativePreview.hpp"

namespace tapioca::playground {
struct Workspace {
    Value document;
    NativePreview native;
    av::ui::GalleryState gallery;
    char search[128] = {};
    bool demo = false, styleEditor = false;
    bool tableOpen = false, colourOpen = false, numberOpen = false;
    av::hudprogramme::TableEdit table;
    av::floorprogramme::Programme tableDraft;
    av::hudprogramme::ColourEdit colour;
    float colourDraft[3] = {};
    av::massingareas::NumberEdit number;
    double numberDraft = 0;
    std::string editError;
};
bool Restore (Workspace& workspace, const Value& document);
void DrawWorkspace (Workspace& workspace, const TapiocaUiFrame& frame, TapiocaUiResult& result);
} // namespace tapioca::playground
