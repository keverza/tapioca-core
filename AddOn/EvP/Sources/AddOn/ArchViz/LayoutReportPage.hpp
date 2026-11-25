#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz {
// Planting (#278) is its own page: the Planting tab's kept layers, never the
// three seasonal days the sun pages trace, so "Capture all 3" leaves it out.
enum class ReportTemplate { Analysis, Patterns, Access, Planting };
constexpr int kSunReportPages = 3;
constexpr int kReportPages = 4;
constexpr size_t kPlantingMaxTiles = 4;
struct ReportFormat {
    unsigned width, height;
};
inline ReportFormat ReportPageFormat (int preset)
{
    constexpr ReportFormat pages[] = { { 1754, 1240 }, { 1240, 1754 }, { 2480, 1754 }, { 1754, 2480 } };
    return pages[preset >= 0 && preset < 4 ? preset : 0]; // ISO paper at 150 dpi
}
inline const char* ReportTitle (ReportTemplate t)
{
    return t == ReportTemplate::Analysis   ? "Shadow Analysis"
           : t == ReportTemplate::Patterns ? "Shadow Patterns"
           : t == ReportTemplate::Planting ? "Planting Suitability"
                                           : "Sunlight Access";
}
struct ReportPanel {
    std::string date, season, window, metric;
    unsigned width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
// One row of the planting page's area table: a layer's plan m2 per class,
// unavailable and measured.
struct ReportAreaRow {
    std::string name;
    std::vector<double> classAreas;
    double unavailable = 0.0, measured = 0.0;
};
struct LayoutReportPage {
    ReportTemplate type = ReportTemplate::Analysis;
    int format = 0;
    std::string site;
    std::vector<int> clocks;
    std::vector<ReportPanel> panels;
    float tileAspect = 0;
    unsigned legendHeight = 0;
    bool pending = false;
    std::vector<uint8_t> legend;
    // Planting (#278): the subtitle, the class columns, one area row per tile
    // and the notes above the legend strip. Panels: `date` is the layer's
    // name, `metric` its class shares.
    std::string subtitle;
    std::vector<std::string> classNames;
    std::vector<ReportAreaRow> areaRows;
    std::vector<std::string> notes;
};
// Called by both the viewer and RenderBench. Source pixels are the settled scene
// readbacks; the physical page never changes the camera or measurement domain.
struct PaperViewportRect {
    float x, y, w, h;
}; // normalized paper coordinates
bool ComposeReportPage (const LayoutReportPage& page, std::vector<uint8_t>& rgba,
                        std::vector<PaperViewportRect>* viewports = nullptr);
void StoreReportTile (ReportPanel& panel, std::vector<uint8_t>& source);
} // namespace geomsrv::archviz
