#ifndef EVP_ARCHVIZ_OVERLAYHOVERDWELL_HPP
#define EVP_ARCHVIZ_OVERLAYHOVERDWELL_HPP

#include <cstddef>
#include <cstdint>

namespace geomsrv::archviz::overlayhover3d {

// Pure same-cell dwell policy. The caller owns the clock, retained layer and timer.
class CellDwell {
  public:
    static constexpr uint64_t kDelayMilliseconds = 500;

    bool Observe (uintptr_t layer, size_t mesh, size_t cell, uint64_t now)
    {
        if (!active_ || layer != layer_ || mesh != mesh_ || cell != cell_ || now < since_) {
            active_ = true;
            layer_ = layer;
            mesh_ = mesh;
            cell_ = cell;
            since_ = now;
        }
        return Remaining (now) == 0;
    }

    uint64_t Remaining (uint64_t now) const
    {
        if (!active_ || now < since_)
            return kDelayMilliseconds;
        const uint64_t elapsed = now - since_;
        return elapsed >= kDelayMilliseconds ? 0 : kDelayMilliseconds - elapsed;
    }

    void Reset ()
    {
        active_ = false;
    }

  private:
    bool active_ = false;
    uintptr_t layer_ = 0;
    size_t mesh_ = 0, cell_ = 0;
    uint64_t since_ = 0;
};

} // namespace geomsrv::archviz::overlayhover3d

#endif
