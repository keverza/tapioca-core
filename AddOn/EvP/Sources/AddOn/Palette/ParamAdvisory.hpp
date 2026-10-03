#ifndef EVP_PALETTE_PARAMADVISORY_HPP
#define EVP_PALETTE_PARAMADVISORY_HPP

#include "DGModule.hpp"
#include "Geometry/Mesh.hpp"
#include <chrono>
#include <memory>
#include <vector>

namespace evp {
class PaletteScroll;

// Optional, declarative advice below a parameter. It observes the immutable
// captured model; never extracts geometry or changes the user's chosen value.
class ParamAdvisory {
  public:
    explicit ParamAdvisory (const DG::Panel& panel);
    bool Refresh (const GS::UniString& selected);
    short PlaceAt (short top, short left, short right, const PaletteScroll& clip);
    void Hide ();

  private:
    const DG::Panel& panel;
    const geomsrv::Snapshot* snapshot = nullptr; // identity only, never dereferenced
    uint64_t snapshotId = 0;
    size_t triangles = 0;
    GS::UniString selectedValue;
    std::chrono::steady_clock::time_point nextRefresh;
    GS::UniString text;
    GS::UniString wrappedText;
    short wrappedWidth = 0;
    std::vector<std::unique_ptr<DG::LeftText>> lines;
};
} // namespace evp
#endif
