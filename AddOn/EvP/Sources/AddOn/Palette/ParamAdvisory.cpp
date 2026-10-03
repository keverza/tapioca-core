#include "Palette/ParamAdvisory.hpp"
#include "Palette/PaletteScroll.hpp"
#include "Geometry/MeshStore.hpp"
#include "SunStudy/SunStudyPreset.hpp"
#include "ArchViz/DiligentViewport.hpp"
#include "ArchViz/ExtractionThread.hpp"

#include <algorithm>

namespace evp {

ParamAdvisory::ParamAdvisory (const DG::Panel& panel) : panel (panel)
{
}

bool ParamAdvisory::Refresh (const GS::UniString& selected)
{
    const auto now = std::chrono::steady_clock::now ();
    if (selected == selectedValue && now < nextRefresh)
        return false;
    selectedValue = selected;
    nextRefresh = now + std::chrono::milliseconds (250);
    const auto current = geomsrv::MeshStore::Get ().Current ();
    const uint64_t currentId = current != nullptr ? current->id : 0;
    if (current.get () != snapshot || currentId != snapshotId) {
        snapshot = current.get ();
        snapshotId = currentId;
        triangles = current != nullptr && current->scope == "all" ? current->TotalTriangles () : 0;
    }
    // A loaded viewer supplies guidance even before the first BuildSnapshot.
    // Ignore partial/in-flight scenes rather than call a large model small.
    size_t knownTriangles = triangles;
    if (current == nullptr || current->scope != "all") {
        const auto stats = geomsrv::archviz::DiligentViewport::Get ().Stats ();
        knownTriangles = stats.running && stats.sceneReady && stats.scenePending == 0 &&
                                 !geomsrv::archviz::ExtractionWorker::Get ().IsRunning ()
                             ? static_cast<size_t> (stats.sceneTriangles)
                             : 0;
    }
    const std::string preset = selected == "early model" ? "early" : selected == "late model" ? "late" : "";
    const GS::UniString next (evp::sunstudy::SunStudyPresetAdvice (knownTriangles, preset).c_str (), CC_UTF8);
    if (next == text)
        return false;
    text = next;
    return true;
}

short ParamAdvisory::PlaceAt (short top, short left, short right, const PaletteScroll& clip)
{
    constexpr short lineHeight = 16;
    const short width = right - left;
    if (wrappedText != text || wrappedWidth != width) {
        lines.clear ();
        wrappedText = text;
        wrappedWidth = width;
        const USize perLine = static_cast<USize> (std::max<short> (20, width / 7));
        GS::Array<GS::UniString> words;
        text.Split (" ", &words);
        GS::UniString line;
        const auto flush = [&] {
            if (line.IsEmpty ())
                return;
            auto label = std::make_unique<DG::LeftText> (panel, DG::Rect (left, 0, right, lineHeight));
            label->SetText (line);
            label->SetTextColor (Gfx::Color (static_cast<unsigned char> (220), static_cast<unsigned char> (120),
                                             static_cast<unsigned char> (20)));
            label->Hide ();
            lines.push_back (std::move (label));
            line.Clear ();
        };
        for (const auto& word : words) {
            if (!line.IsEmpty () && line.GetLength () + word.GetLength () + 1 > perLine)
                flush ();
            line += line.IsEmpty () ? word : GS::UniString (" ") + word;
        }
        flush ();
    }
    short y = top;
    for (auto& line : lines) {
        clip.Place (line.get (), DG::Rect (left, y, right, static_cast<short> (y + lineHeight)));
        y += lineHeight;
    }
    return lines.empty () ? 0 : static_cast<short> (y - top + 4);
}

void ParamAdvisory::Hide ()
{
    for (auto& line : lines)
        line->Hide ();
}

} // namespace evp
