#include "ArchViz/MaterialTable.hpp"

namespace geomsrv {
namespace archviz {

const SurfaceMaterial& MaterialTable::Missing ()
{
    // Function-local static: one instance, no static-init order to reason about,
    // and a reference that stays valid for the process. See the header for why
    // it is white rather than a debug colour.
    static const SurfaceMaterial white;
    return white;
}

void MaterialTable::Set (const SurfaceMaterial& m)
{
    for (SurfaceMaterial& existing : byIndex_) {
        if (existing.index == m.index) {
            existing = m;
            return;
        }
    }
    byIndex_.push_back (m);
}

const SurfaceMaterial& MaterialTable::Lookup (int32_t index) const
{
    for (const SurfaceMaterial& m : byIndex_) {
        if (m.index == index)
            return m;
    }
    return Missing ();
}

bool MaterialTable::Has (int32_t index) const
{
    for (const SurfaceMaterial& m : byIndex_) {
        if (m.index == index)
            return true;
    }
    return false;
}

size_t MaterialTable::Bytes () const
{
    size_t n = sizeof (MaterialTable);
    for (const SurfaceMaterial& m : byIndex_)
        n += m.Bytes ();
    return n;
}

}   // namespace archviz
}   // namespace geomsrv
