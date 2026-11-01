#ifndef EVP_ARCHVIZ_OVERLAYRAMPCHOICES_HPP
#define EVP_ARCHVIZ_OVERLAYRAMPCHOICES_HPP

// ArchViz/OverlayRampChoices -- the HUD gradient picker's choices for the layer store
// (#102), split from ArchViz/OverlayLayers.cpp, which is their only caller. A choice is
// remembered against the preset the SCRIPT wrote, so a republished layer keeps the
// user's colours; it is session state, forgotten with the session's layers
// (OVERLAY-INVARIANTS.md §8). MAIN THREAD, like the store.

#include "ArchViz/OverlayLayers.hpp"

#include <string>
#include <vector>

namespace geomsrv::archviz::overlaylayers::rampchoices {

// The remembered choices, onto a layer about to be stored.
void Apply (Layer& layer);
// Every colormap of `layer` drawn from `from` takes `to` (custom stops, flipped) and the
// choice is remembered. False when nothing changed or `to` cannot be drawn.
bool Recolour (Layer& layer, const std::string& from, const std::string& to, const std::vector<ColourStop>& custom,
               bool flipped);
// Every choice forgotten: the session's layers are gone.
void Forget ();

} // namespace geomsrv::archviz::overlaylayers::rampchoices

#endif
