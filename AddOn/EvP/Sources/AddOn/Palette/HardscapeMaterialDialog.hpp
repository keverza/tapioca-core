#ifndef TAPIOCA_HARDSCAPE_MATERIAL_DIALOG_HPP
#define TAPIOCA_HARDSCAPE_MATERIAL_DIALOG_HPP
#include "Edit/EditMaterial.hpp"
namespace tapioca::materialui {
// Main thread, out of HUD layout. False with an empty error is Cancel.
bool Pick (const geometry::MaterialSpec& before, const geometry::MaterialCatalog& catalog,
           geometry::MaterialSpec& picked, std::string& error);
} // namespace tapioca::materialui
#endif
