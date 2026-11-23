#ifndef EVP_ARCHVIZ_VIEWERSCREENSHOTGEOMETRY_HPP
#define EVP_ARCHVIZ_VIEWERSCREENSHOTGEOMETRY_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace geomsrv::archviz {

constexpr int kShotSunStudy = 0;
constexpr int kShotExposedSurfaces = 1;
constexpr int kShotVision = 2;
constexpr float kViewerScreenshotScales[] = { 1.5f, 2.0f, 3.0f };
constexpr const char* kViewerScreenshotScaleLabels[] = { "1.5x", "2x", "3x" };
constexpr int kViewerScreenshotScaleCount = 3;
constexpr uint32_t kViewerScreenshotMaxDimension = 16384;

struct ScreenshotRect {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;

    float Width () const
    {
        return right - left;
    }
    float Height () const
    {
        return bottom - top;
    }
};

struct ScreenshotSize {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct ScreenshotPixels {
    uint32_t left = 0;
    uint32_t top = 0;
    uint32_t right = 0;
    uint32_t bottom = 0;

    uint32_t Width () const
    {
        return right - left;
    }
    uint32_t Height () const
    {
        return bottom - top;
    }
};

// Keep the full-view aspect even when the larger dimension reaches the texture limit.
ScreenshotSize ViewerScreenshotTargetSize (uint32_t width, uint32_t height, float scale);
ScreenshotPixels ViewerScreenshotCrop (ScreenshotRect normalized, ScreenshotSize size);
bool CropScreenshotRgba (std::vector<uint8_t>& image, ScreenshotSize size, ScreenshotPixels crop);
std::vector<int> DaylightPanelSteps (const std::vector<uint16_t>& minutes, int hourStep);
ScreenshotSize TimeGrid (size_t count, ScreenshotSize canvas);
struct SeasonalTile {
    ScreenshotPixels image;
    uint32_t gutter = 0, header = 0, gap = 0, caption = 0;
};
SeasonalTile SeasonalTileOf (ScreenshotSize canvas, unsigned columns, unsigned index, float scale);
bool PlaceTimePanel (std::vector<uint8_t>& layout, ScreenshotSize panel, size_t count, size_t index, uint32_t caption,
                     const std::vector<uint8_t>& pixels, ScreenshotSize grid = {}, ScreenshotPixels destination = {});
std::filesystem::path ViewerScreenshotPath (const std::filesystem::path& directory, int kind, float scale,
                                            const char* timestamp, std::error_code& error);

} // namespace geomsrv::archviz

#endif
