#ifndef EVP_ARCHVIZ_MASSINGCALCULATION_HPP
#define EVP_ARCHVIZ_MASSINGCALCULATION_HPP

#include "ArchViz/MassingRules.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "Geometry/Mesh.hpp"
#include <memory>
#include <optional>

namespace geomsrv::archviz::massingcalculation {
enum class Action { Calculate, Cancel, Clear };
struct Request {
    Action action = Action::Calculate;
    massingrules::Page before;
    std::vector<massingrules::Assignment> assignments;
    std::vector<bool> regulated, endpoints;
    int landscape = 0; // 0 = Existing, 1 = New; no implicit fallback/terrain fusion
    double baseHeight = 8.5, runPerRise = 0.5, capZ = 25, baseDepth = 1;
    bool capped = true;
};
struct Result {
    overlaylayers::Layer layer;
    overlaylayers::Layer site;
    std::vector<double> offsetXY;
    bool hasEnvelope = false;
    std::string note;
    double parcelArea = 0, allowedArea = 0, meanZ = 0, meanASL = 0;
    bool hasMeanZ = false, hasMeanASL = false;
    uint32_t faces = 0;
};
struct Preview {
    Request inputs;
    Result result;
};
bool SameRequest (const Request& a, const Request& b);
// Coalesce slider bursts and edits during a running Python call. Revision changes
// immediately, so a superseded completion can never publish, even before debounce.
class PreviewQueue {
  public:
    bool Follow (Request request, uint64_t now);
    void Refresh (uint64_t now);
    void Reset ();
    std::optional<Request> TakeReady (uint64_t now, bool busy);
    const std::optional<Request>& Desired () const
    {
        return desired;
    }
    uint64_t Revision () const
    {
        return revision;
    }
    bool Pending () const
    {
        return pending;
    }

  private:
    std::optional<Request> desired;
    uint64_t revision = 0, due = 0;
    bool pending = false;
};
// Pure snapshot/JSON adapters. No Python or ACAPI during native widget layout.
bool Encode (const Request& request, const geomsrv::Mesh& terrain, bool hasAltitude, double altitude, std::string& json,
             std::string& error);
bool Decode (const std::string& bridgeJson, Result& result, std::string& error);
constexpr char kDimensions[] = "tapioca.massing.offsetDimensions";
overlaylayers::Layer OffsetDimensions (const Preview& preview);
} // namespace geomsrv::archviz::massingcalculation
#endif
