#ifndef EVP_NATIVECOMMANDS_MODELAPPEARANCECOMMANDS_HPP
#define EVP_NATIVECOMMANDS_MODELAPPEARANCECOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// E24 — the model's appearance pool: GetModelMaterials, GetModelColors,
// GetModelTextures, GetTexturePixels, GetModelLights, GetTextureCoordinates.
// What the polygon `materialIndex` from EvP.GetBodyGeometry actually points at.
// Returns the domain's ordered command registrations.
NativeCommandRegistrations GetModelAppearanceCommandRegistrations ();

} // namespace geomsrv

#endif
