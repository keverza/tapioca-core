#ifndef TAPIOCA_NATIVE_HARDSCAPE_MATERIAL_HPP
#define TAPIOCA_NATIVE_HARDSCAPE_MATERIAL_HPP
#include "Edit/EditMaterial.hpp"
namespace tapioca::materialui {
// Main-thread project read. Shared by the picker and material availability guard.
bool ReadCatalog (geometry::MaterialCatalog& catalog, std::string& error);
} // namespace tapioca::materialui
#endif
