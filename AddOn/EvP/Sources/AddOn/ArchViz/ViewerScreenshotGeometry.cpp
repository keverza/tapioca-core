#include "ArchViz/ViewerScreenshotGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace geomsrv::archviz {

ScreenshotSize ViewerScreenshotTargetSize (uint32_t width, uint32_t height, float scale)
{
    if (width == 0 || height == 0 || !std::isfinite (scale) || scale <= 0.0f)
        return {};
    const double limited =
        (std::min) (double (scale), double (kViewerScreenshotMaxDimension) / double ((std::max) (width, height)));
    return { uint32_t ((std::max) (1L, std::lround (width * limited))),
             uint32_t ((std::max) (1L, std::lround (height * limited))) };
}

ScreenshotPixels ViewerScreenshotCrop (ScreenshotRect rect, ScreenshotSize size)
{
    if (!std::isfinite (rect.left) || !std::isfinite (rect.top) || !std::isfinite (rect.right) ||
        !std::isfinite (rect.bottom))
        return {};
    const auto edge = [] (float value, uint32_t extent) {
        return uint32_t (std::llround (double (std::clamp (value, 0.0f, 1.0f)) * extent));
    };
    const ScreenshotPixels crop = { edge (rect.left, size.width), edge (rect.top, size.height),
                                    edge (rect.right, size.width), edge (rect.bottom, size.height) };
    return crop.left < crop.right && crop.top < crop.bottom ? crop : ScreenshotPixels {};
}

bool CropScreenshotRgba (std::vector<uint8_t>& image, ScreenshotSize size, ScreenshotPixels crop)
{
    if (size.width > kViewerScreenshotMaxDimension || size.height > kViewerScreenshotMaxDimension ||
        crop.left >= crop.right || crop.top >= crop.bottom || crop.right > size.width || crop.bottom > size.height)
        return false;
    constexpr size_t bytesPerPixel = 4;
    const size_t stride = size_t (size.width) * bytesPerPixel;
    if (image.size () != stride * size.height)
        return false;
    const size_t rowBytes = size_t (crop.Width ()) * bytesPerPixel;
    for (uint32_t row = 0; row < crop.Height (); ++row) {
        const size_t source = size_t (crop.top + row) * stride + size_t (crop.left) * bytesPerPixel;
        std::memmove (image.data () + size_t (row) * rowBytes, image.data () + source, rowBytes);
    }
    image.resize (rowBytes * crop.Height ());
    return true;
}

bool PlaceTimePanel (std::vector<uint8_t>& layout, ScreenshotSize panel, size_t count, size_t index,
                     const std::vector<uint8_t>& pixels)
{
    if (!panel.width || !panel.height || count < 2 || count > 4 || index >= count ||
        panel.width > kViewerScreenshotMaxDimension / count || panel.height > kViewerScreenshotMaxDimension)
        return false;
    const size_t stride = size_t (panel.width) * 4;
    if (pixels.size () != stride * panel.height || layout.size () != pixels.size () * count)
        return false;
    for (uint32_t y = 0; y < panel.height; ++y)
        std::memcpy (layout.data () + (y * count + index) * stride, pixels.data () + y * stride, stride);
    return true;
}

std::filesystem::path ViewerScreenshotPath (const std::filesystem::path& directory, int kind, float scale,
                                            const char* timestamp, std::error_code& error)
{
    const char* prefix = kind == kShotVision            ? "vision"
                         : kind == kShotExposedSurfaces ? "exposed-surfaces"
                                                        : "sunstudy";
    char stem[96] = {};
    std::snprintf (stem, sizeof (stem), "%s-%s-%gx", prefix, timestamp, double (scale));
    std::filesystem::path path = directory / (std::string (stem) + ".png");
    for (unsigned suffix = 2; std::filesystem::exists (path, error); ++suffix)
        path = directory / (std::string (stem) + "-" + std::to_string (suffix) + ".png");
    return path;
}

} // namespace geomsrv::archviz
