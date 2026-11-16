// ArchViz/OverlayHudCriteriaTable: the exposed-surface criteria grid keeps every column
// legible. A panel narrower than the columns' minimum scrolls the table sideways; a
// wider one stretches criterion, condition and value in proportion.

#include "ArchViz/OverlayHudCriteriaTable.hpp"

#include <gtest/gtest.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <map>
#include <string>
#include <vector>

namespace hud = geomsrv::archviz::overlayhud;
namespace layers = geomsrv::archviz::overlaylayers;

namespace {

struct Harness {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    std::map<std::string, hud::CriteriaText> texts;
    layers::PanelItem grid;

    Harness ()
    {
        ImGuiIO& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = { 1600, 900 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
        grid.kind = layers::ItemKind::CriteriaGrid;
        grid.labels = { "Element Type", "Layer", "Home Story", "Element ID", "Surface", "Face direction" };
        grid.columns = { "is", "is not", "contains", "starts with", "ends with" };
        for (int i = 0; i < 3; ++i) {
            layers::CriteriaRow row;
            row.id = "rule:" + std::to_string (i);
            row.field = uint32_t (i == 1 ? 5 : 0);
            row.condition = 3;
            row.value = "Stucco";
            grid.criteria.push_back (row);
        }
    }
    ~Harness ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    struct Seen {
        hud::CriteriaWidths minimum;
        std::vector<float> columns;
        float outer = 0, inner = 0, scrollMax = 0;
    };
    // Two frames at `width`: tables settle their column widths on the second.
    Seen Draw (float width)
    {
        Seen seen;
        for (int frame = 0; frame < 3; ++frame) {
            ImGui::NewFrame ();
            ImGui::SetNextWindowPos ({ 0, 0 });
            ImGui::SetNextWindowSize ({ width + 2 * ImGui::GetStyle ().WindowPadding.x, 600 });
            ImGui::Begin ("panel", nullptr, ImGuiWindowFlags_NoDecoration);
            std::vector<hud::CriteriaEdit> edits;
            bool active = false;
            seen.minimum = hud::MeasureCriteria (grid);
            hud::DrawCriteriaTable (grid, texts, ImGui::GetContentRegionAvail ().x, edits, active);
            ImGuiTable* table = ImGui::TableFindByID (ImGui::GetID ("##criteriaRules"));
            if (table) {
                seen.columns.clear ();
                for (int i = 0; i < table->ColumnsCount; ++i)
                    seen.columns.push_back (table->Columns[i].WidthGiven);
                seen.outer = table->OuterRect.GetWidth ();
                seen.inner = table->InnerWidth;
                seen.scrollMax = table->InnerWindow ? table->InnerWindow->ScrollMax.x : 0;
            }
            ImGui::End ();
            ImGui::Render ();
        }
        return seen;
    }
};

} // namespace

TEST (OverlayHudCriteriaTable, NarrowPanelScrollsSidewaysInsteadOfCrushingColumns)
{
    Harness harness;
    const auto seen = harness.Draw (300);
    ASSERT_EQ (seen.columns.size (), 8u);
    EXPECT_GT (seen.minimum.minimum, 300.0f);
    EXPECT_NEAR (seen.outer, 300.0f, 1.0f);
    EXPECT_GT (seen.scrollMax, 0.0f) << "the section alone scrolls horizontally";
    const float label = ImGui::CalcTextSize ("Face direction").x;
    EXPECT_GE (seen.columns[1], label) << "criterion shows its longest choice";
    EXPECT_GE (seen.columns[2], ImGui::CalcTextSize ("starts with").x);
    EXPECT_GE (seen.columns[3], 9 * ImGui::GetFontSize ());
    for (float width : seen.columns)
        EXPECT_GT (width, ImGui::GetFontSize ());
}

TEST (OverlayHudCriteriaTable, WidePanelStretchesCriterionConditionAndValueInProportion)
{
    Harness harness;
    const auto seen = harness.Draw (1400);
    ASSERT_EQ (seen.columns.size (), 8u);
    EXPECT_LT (seen.minimum.minimum, 1400.0f);
    EXPECT_FLOAT_EQ (seen.scrollMax, 0.0f);
    // Weighted by their own minimum: each grows in proportion to what it must show.
    EXPECT_NEAR (seen.columns[3] / seen.columns[1], seen.minimum.value / seen.minimum.field, 0.05f);
    EXPECT_NEAR (seen.columns[2] / seen.columns[1], seen.minimum.condition / seen.minimum.field, 0.05f);
    EXPECT_GE (seen.columns[3], seen.columns[1]);
    // The fixed controls keep their measure; the extra width goes to the text columns.
    EXPECT_LT (seen.columns[0], seen.columns[2]);
    EXPECT_LT (seen.columns[7], seen.columns[1]);
}
