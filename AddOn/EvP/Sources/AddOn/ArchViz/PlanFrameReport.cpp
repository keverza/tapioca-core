// ArchViz/PlanFrameReport -- see the header. WORKER THREAD: no ACAPI, no D3D, no
// window; Win32 file and clock calls only.

#include "ArchViz/PlanFrameReport.hpp"

#include "ArchViz/PlanFrameRegistration.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace geomsrv {
namespace archviz {
namespace planframes {

namespace {

namespace rec = dxgi::planframes;

// printf into a growing string: one call per row keeps the writer as short as
// the record is wide.
void Append (std::string& out, const char* format, ...)
{
    char buffer[1024];
    va_list arguments;
    va_start (arguments, format);
    const int written = std::vsnprintf (buffer, sizeof (buffer), format, arguments);
    va_end (arguments);
    if (written > 0)
        out.append (buffer, size_t (std::min (written, int (sizeof (buffer)) - 1)));
}

std::string Quoted (const std::string& text)
{
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\')
            out.push_back ('\\');
        if (c >= 0 && c < 0x20)
            continue;
        out.push_back (c);
    }
    out.push_back ('"');
    return out;
}

// %.10g, or null for a value JSON cannot carry.
std::string Number (double value)
{
    if (!std::isfinite (value))
        return "null";
    char buffer[32];
    std::snprintf (buffer, sizeof (buffer), "%.10g", value);
    return buffer;
}

std::string Narrow (const std::wstring& wide)
{
    std::string out;
    for (wchar_t c : wide)
        out.push_back (c < 128 ? char (c) : '?');
    return out;
}

void AppendChain (std::string& out, const ChainRow& row)
{
    Append (out,
            "{\"chain\":\"0x%llx\",\"window\":\"0x%llx\",\"presents\":%llu,\"width\":%u,\"height\":%u,\"format\":%u,"
            "\"swapEffect\":%u,\"bufferCount\":%u,\"flags\":%u,\"ours\":%s,",
            (unsigned long long) row.chain, (unsigned long long) row.window, (unsigned long long) row.presents,
            row.width, row.height, row.format, row.swapEffect, row.bufferCount, row.flags, row.ours ? "true" : "false");
    out += "\"windowClass\":" + Quoted (row.windowClass) + ",\"relation\":" + Quoted (row.relation) + "}";
}

// previous = A * current, in back-buffer PHYSICAL pixels:
//     X' = a*X - b*Y + tx,   Y' = b*X + a*Y + ty
struct PairRow {
    uint32_t frame = 0; // the later of the two
    uint32_t serialGap = 0;
    bool valid = false;
    std::string why;
    double a = 1.0, b = 0.0, tx = 0.0, ty = 0.0;
    double residual = 0.0; // samples of the kept image
    uint32_t textured = 0, used = 0;
};

std::vector<PairRow> RegisterPairs (const ReportInput& input)
{
    std::vector<PairRow> pairs;
    const uint32_t d = rec::kDownsample;
    const size_t frameBytes = size_t (rec::kFrameWidth) * rec::kFrameHeight;
    for (size_t i = 1; i < input.frames.size (); ++i) {
        const rec::FrameInfo& previous = input.frames[i - 1];
        const rec::FrameInfo& current = input.frames[i];
        PairRow pair;
        pair.frame = uint32_t (i);
        pair.serialGap = current.targetSerial - previous.targetSerial;
        if (previous.width != current.width || previous.height != current.height || previous.cropX != current.cropX ||
            previous.cropY != current.cropY || input.framePixels.size () < (i + 1) * frameBytes) {
            pair.why = "the crop changed between the two frames";
            pairs.push_back (pair);
            continue;
        }
        GreyImage a, b;
        a.pixels = input.framePixels.data () + (i - 1) * frameBytes;
        b.pixels = input.framePixels.data () + i * frameBytes;
        a.width = b.width = current.width;
        a.height = b.height = current.height;
        a.stride = b.stride = rec::kFrameWidth;
        const FrameMotion motion = MeasureFrameMotion (a, b);
        pair.valid = motion.valid;
        pair.why = motion.why;
        pair.residual = motion.residual;
        pair.textured = motion.patchesTextured;
        pair.used = motion.patchesUsed;
        // Sample u is centred on physical X = cropX + d*u + d/2.
        const double cx = double (current.cropX) + 0.5 * double (d);
        const double cy = double (current.cropY) + 0.5 * double (d);
        pair.a = motion.a;
        pair.b = motion.b;
        pair.tx = cx - motion.a * cx + motion.b * cy + double (d) * motion.offsetX;
        pair.ty = cy - motion.b * cx - motion.a * cy + double (d) * motion.offsetY;
        pairs.push_back (pair);
    }
    return pairs;
}

bool WriteFile (const std::wstring& path, const void* header, size_t headerBytes, const void* data, size_t bytes)
{
    FILE* file = _wfopen (path.c_str (), L"wb");
    if (file == nullptr)
        return false;
    bool ok = true;
    if (headerBytes > 0)
        ok = std::fwrite (header, 1, headerBytes, file) == headerBytes;
    if (ok && bytes > 0)
        ok = std::fwrite (data, 1, bytes, file) == bytes;
    ok = std::fclose (file) == 0 && ok;
    return ok;
}

} // namespace

ReportResult WriteReport (const ReportInput& input)
{
    ReportResult result;
    const ULONGLONG began = ::GetTickCount64 ();
    const std::vector<PairRow> pairs = RegisterPairs (input);
    result.pairs = uint32_t (pairs.size ());
    for (const PairRow& pair : pairs)
        if (pair.valid)
            ++result.pairsValid;

    const std::wstring stamp (input.stamp.begin (), input.stamp.end ());
    const std::wstring framesFile = L"planframes_" + stamp + L".bin";

    std::string json;
    json.reserve (8u << 20);
    Append (json, "{\"version\":2,\"reason\":%s,\"qpcFrequency\":%lld,\"startQpc\":%lld,\"mainThread\":%u,\"dpi\":%s,",
            Quoted (input.reason).c_str (), (long long) input.qpcFrequency, (long long) input.startQpc,
            input.mainThread, Number (input.dpi).c_str ());
    Append (json,
            "\"canvas\":{\"window\":\"0x%llx\",\"class\":%s,\"width\":%u,\"height\":%u,\"logicalWidth\":%u,"
            "\"logicalHeight\":%u,\"placementKnown\":%s,\"offsetX\":%d,\"offsetY\":%d,\"targetClientWidth\":%u,"
            "\"targetClientHeight\":%u},",
            (unsigned long long) input.canvasWindow, Quoted (input.canvasClass).c_str (), input.canvasWidth,
            input.canvasHeight, input.logicalWidth, input.logicalHeight, input.placementKnown ? "true" : "false",
            input.offsetX, input.offsetY, input.targetClientWidth, input.targetClientHeight);
    json += "\"target\":";
    if (input.target.chain != 0)
        AppendChain (json, input.target);
    else
        json += "null";
    json += ",\"chains\":[";
    for (size_t i = 0; i < input.chains.size (); ++i) {
        if (i > 0)
            json += ",";
        AppendChain (json, input.chains[i]);
    }
    const rec::Stats& s = input.stats;
    Append (json,
            "],\"capture\":{\"presentsDropped\":%llu,\"targetPresents\":%llu,\"framesSubmitted\":%llu,"
            "\"framesReady\":%llu,\"slotsBusy\":%llu,\"readbackFailures\":%llu,\"unsupportedFormat\":%llu,"
            "\"targetChanges\":%llu,\"samplesDropped\":%llu,\"canvasMessages\":%llu,\"retrievedMessages\":%llu,"
            "\"idleRedraws\":%u},",
            (unsigned long long) s.presentsDropped, (unsigned long long) s.targetPresents,
            (unsigned long long) s.framesSubmitted, (unsigned long long) s.framesReady,
            (unsigned long long) s.slotsBusy, (unsigned long long) s.readbackFailures,
            (unsigned long long) s.unsupportedFormat, (unsigned long long) s.targetChanges,
            (unsigned long long) input.samplesDropped, (unsigned long long) input.canvasMessages,
            (unsigned long long) input.retrievedMessages, input.idleRedraws);

    json += "\"presentColumns\":[\"qpc\",\"chain\",\"thread\",\"mainThread\",\"target\",\"targetSerial\",\"present1\","
            "\"flags\",\"syncInterval\",\"dirtyRects\",\"scroll\",\"scrollX\",\"scrollY\",\"canvasDepth\","
            "\"canvasMessage\",\"canvasSerial\",\"latestSample\",\"retrievedMessage\",\"retrievedWindow\","
            "\"retrievedSerial\",\"frame\",\"tookUs\",\"atPresent\"],\"presents\":[";
    for (size_t i = 0; i < input.presents.size (); ++i) {
        const rec::PresentRecord& r = input.presents[i];
        Append (
            json, "%s[%lld,\"0x%llx\",%u,%d,%d,%u,%d,%u,%u,%u,%d,%d,%d,%u,%u,%llu,%llu,%u,\"0x%llx\",%llu,%d,%u,%llu]",
            i > 0 ? "," : "", (long long) r.qpc, (unsigned long long) r.chain, r.thread, r.mainThread ? 1 : 0,
            r.target ? 1 : 0, r.targetSerial, r.present1 ? 1 : 0, r.flags, r.syncInterval, r.dirtyRects,
            r.scroll ? 1 : 0, r.scrollX, r.scrollY, r.canvasDepth, r.canvasMessage, (unsigned long long) r.canvasSerial,
            (unsigned long long) r.latestSample, r.retrievedMessage, (unsigned long long) r.retrievedWindow,
            (unsigned long long) r.retrievedSerial, r.frame, r.tookUs, (unsigned long long) r.atPresent);
    }
    json += "],\"sampleColumns\":[\"serial\",\"qpc\",\"source\",\"message\",\"depth\",\"messageSerial\",\"valid\","
            "\"torn\",\"costUs\",\"xx\",\"xy\",\"yx\",\"yy\",\"ox\",\"oy\",\"error\"],\"sampleSources\":[\"entry\","
            "\"exit\",\"timer\",\"present\"],\"samples\":[";
    for (size_t i = 0; i < input.samples.size (); ++i) {
        const TransformSample& t = input.samples[i];
        Append (json, "%s[%llu,%lld,%u,%u,%u,%llu,%d,%d,%u,%s,%s,%s,%s,%s,%s,%d]", i > 0 ? "," : "",
                (unsigned long long) t.serial, (long long) t.qpc, uint32_t (t.source), t.message, t.depth,
                (unsigned long long) t.messageSerial, t.valid ? 1 : 0, t.torn ? 1 : 0, t.costUs, Number (t.xx).c_str (),
                Number (t.xy).c_str (), Number (t.yx).c_str (), Number (t.yy).c_str (), Number (t.ox).c_str (),
                Number (t.oy).c_str (), t.error);
    }
    Append (json,
            "],\"downsample\":%u,\"frameColumns\":[\"present\",\"targetSerial\",\"qpc\",\"cropX\",\"cropY\",\"width\","
            "\"height\",\"bufferWidth\",\"bufferHeight\",\"format\"],\"frames\":[",
            rec::kDownsample);
    for (size_t i = 0; i < input.frames.size (); ++i) {
        const rec::FrameInfo& f = input.frames[i];
        Append (json, "%s[%u,%u,%lld,%u,%u,%u,%u,%u,%u,%u]", i > 0 ? "," : "", f.presentIndex, f.targetSerial,
                (long long) f.qpc, f.cropX, f.cropY, f.width, f.height, f.bufferWidth, f.bufferHeight, f.format);
    }
    json += "],\"pairColumns\":[\"frame\",\"serialGap\",\"valid\",\"a\",\"b\",\"tx\",\"ty\",\"residual\",\"textured\","
            "\"used\",\"why\"],\"pairs\":[";
    for (size_t i = 0; i < pairs.size (); ++i) {
        const PairRow& p = pairs[i];
        Append (json, "%s[%u,%u,%d,%s,%s,%s,%s,%s,%u,%u,%s]", i > 0 ? "," : "", p.frame, p.serialGap, p.valid ? 1 : 0,
                Number (p.a).c_str (), Number (p.b).c_str (), Number (p.tx).c_str (), Number (p.ty).c_str (),
                Number (p.residual).c_str (), p.textured, p.used, Quoted (p.why).c_str ());
    }
    json += "],\"evaluation\":";
    // ⚠️ THE PATCH GRID'S EXTENT, physical pixels: where the fit was measured.
    // Outside it the fit is an extrapolation, and the diagnostic never reads one.
    if (!input.frames.empty ()) {
        const rec::FrameInfo& f = input.frames.front ();
        const double d = double (rec::kDownsample);
        auto at = [d] (uint32_t crop, double sample) { return double (crop) + d * sample + 0.5 * d; };
        Append (json, "{\"left\":%s,\"top\":%s,\"right\":%s,\"bottom\":%s}",
                Number (at (f.cropX, double (f.width) / 4.0)).c_str (),
                Number (at (f.cropY, double (f.height) / 4.0)).c_str (),
                Number (at (f.cropX, 3.0 * double (f.width) / 4.0)).c_str (),
                Number (at (f.cropY, 3.0 * double (f.height) / 4.0)).c_str ());
    }
    else {
        json += "null";
    }
    json += ",\"framesFile\":" + Quoted (Narrow (framesFile)) + "}";

    if (!input.directory.empty ()) {
        ::CreateDirectoryW (input.directory.c_str (), nullptr);
        const std::wstring jsonPath = input.directory + L"\\planframes_" + stamp + L".json";
        if (WriteFile (jsonPath, nullptr, 0, json.data (), json.size ()))
            result.jsonPath = Narrow (jsonPath);
        // 'PLNF', version, the store's width and height (every frame is stored at
        // that stride), the frame count; then the frames in order.
        const uint32_t header[5] = { 0x464E4C50u, 1u, rec::kFrameWidth, rec::kFrameHeight,
                                     uint32_t (input.frames.size ()) };
        const std::wstring binPath = input.directory + L"\\" + framesFile;
        if (WriteFile (binPath, header, sizeof (header), input.framePixels.data (), input.framePixels.size ()))
            result.framesPath = Narrow (binPath);
    }
    result.analysisMs = ::GetTickCount64 () - began;
    return result;
}

} // namespace planframes
} // namespace archviz
} // namespace geomsrv
