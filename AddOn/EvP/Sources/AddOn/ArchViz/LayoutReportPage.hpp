#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz {
enum class ReportTemplate { Analysis, Patterns, Access };
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
                                           : "Sunlight Access";
}
struct ReportPanel {
    std::string date, season, window, metric;
    unsigned width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
struct LayoutReportPage {
    ReportTemplate type = ReportTemplate::Analysis;
    int format = 0;
    std::string site;
    std::vector<int> clocks;
    std::vector<ReportPanel> panels;
};
// Called by both the viewer and RenderBench. Source pixels are the settled scene
// readbacks; the physical page never changes the camera or measurement domain.
bool ComposeReportPage (const LayoutReportPage& page, std::vector<uint8_t>& rgba);
void StoreReportTile (ReportPanel& panel, std::vector<uint8_t>& source);
} // namespace geomsrv::archviz
