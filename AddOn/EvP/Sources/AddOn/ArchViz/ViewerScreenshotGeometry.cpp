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

std::vector<int> DaylightPanelSteps (const std::vector<uint16_t>& minutes, int hourStep)
{
    if (minutes.empty () || minutes.back () >= 1440 || hourStep < 1 || hourStep > 6 ||
        !std::is_sorted (minutes.begin (), minutes.end ()))
        return {};
    std::vector<int> steps { 0 };
    for (int time = minutes.front () + hourStep * 60; time < minutes.back (); time += hourStep * 60) {
        const auto it = std::lower_bound (minutes.begin (), minutes.end (), time);
        const int index = int (it - minutes.begin ());
        if (minutes[index] != minutes[steps.back ()])
            steps.push_back (index);
    }
    if (minutes[steps.back ()] != minutes.back ())
        steps.push_back (int (minutes.size () - 1));
    return steps;
}

ScreenshotSize TimeGrid (size_t count, ScreenshotSize canvas)
{
    if (!count || count > 25)
        return {};
    const auto longer = uint32_t (std::ceil (std::sqrt (double (count))));
    const auto shorter = uint32_t ((count + longer - 1) / longer);
    return canvas.width >= canvas.height ? ScreenshotSize { longer, shorter } : ScreenshotSize { shorter, longer };
}

bool PlaceTimePanel (std::vector<uint8_t>& layout, ScreenshotSize panel, size_t count, size_t index, uint32_t caption,
                     const std::vector<uint8_t>& pixels)
{
    const auto grid = TimeGrid (count, panel);
    if (!grid.width || !panel.width || !panel.height || index >= count || panel.width > kViewerScreenshotMaxDimension ||
        panel.height > kViewerScreenshotMaxDimension)
        return false;
    const size_t stride = size_t (panel.width) * 4;
    if (pixels.size () != stride * panel.height || layout.size () != pixels.size ())
        return false;
    const auto col = uint32_t (index % grid.width), row = uint32_t (index / grid.width);
    const auto left = col * panel.width / grid.width, top = row * panel.height / grid.height;
    const auto cw = (col + 1) * panel.width / grid.width - left, ch = (row + 1) * panel.height / grid.height - top;
    if (!cw || ch <= caption)
        return false;
    const auto scale = std::min (double (cw) / panel.width, double (ch - caption) / panel.height);
    const auto w = std::max (1u, uint32_t (std::lround (panel.width * scale))),
               h = std::max (1u, uint32_t (std::lround (panel.height * scale)));
    const auto x0 = left + (cw - w) / 2, y0 = top + (ch - caption - h) / 2;
    // Average the source footprint instead of dropping thin shadows on reduction.
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t x1 = x * panel.width / w, x2 = (x + 1) * panel.width / w, y1 = y * panel.height / h,
                     y2 = (y + 1) * panel.height / h;
            uint64_t sum[4] = {};
            for (auto sy = y1; sy < y2; ++sy)
                for (auto sx = x1; sx < x2; ++sx)
                    for (int k = 0; k < 4; ++k)
                        sum[k] += pixels[sy * stride + sx * 4 + k];
            for (int k = 0; k < 4; ++k)
                layout[(y0 + y) * stride + (x0 + x) * 4 + k] = uint8_t (sum[k] / ((x2 - x1) * (y2 - y1)));
        }
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
