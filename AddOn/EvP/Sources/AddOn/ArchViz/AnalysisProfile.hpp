#ifndef EVP_ARCHVIZ_ANALYSISPROFILE_HPP
#define EVP_ARCHVIZ_ANALYSISPROFILE_HPP

// ArchViz/AnalysisProfile -- the analysis point's height along its path
// (AnalysisPath.hpp): a Z offset above the path, by station, as a smooth cubic
// Bezier spline through anchors. Each anchor carries one handle; the curve leaves
// it along the handle and arrives along its mirror, so the spline is smooth at
// every anchor. Kept on the Polyline as Tapioca metadata (kPropertyKey).
//
// ⚠️ THE HANDLES ARE CLAMPED WHERE THEY ARE USED, NOT ONLY WHERE THEY ARE SET.
// A segment's station must only grow along it, or one station would have two
// heights; that holds while each handle reaches at most half the segment's span,
// so a handle longer than that (a neighbour moved in under it) is scaled back
// along its own direction.
//
// Pure: no ACAPI, no ImGui.

#include "Metadata/TapiocaMetadata.hpp"

#include <string>
#include <utility>
#include <vector>

namespace geomsrv::archviz::analysisprofile {

struct Anchor {
    double station = 0.0; // metres from the path's start
    double z = 0.0;       // metres above the path
    // The handle out of it, toward the next anchor; the one in is its mirror.
    // handleStation >= 0.
    double handleStation = 0.0;
    double handleZ = 0.0;
};

// By station: the first at 0, the last at the path's length. Two at least.
struct Profile {
    std::vector<Anchor> anchors;
};

// A constant `z` over a path of `length`.
Profile Flat (double length, double z);

// Stretched to a path of `length`: stations (and their handles) scaled, the ends
// pinned to 0 and `length`. An unusable profile becomes Flat at its mean height.
void Fit (Profile& profile, double length);

// The offset at `station`, clamped to the profile's span.
double ZAt (const Profile& profile, double station);

// The curve for drawing: `perSegment` steps between anchors, as (station, z).
std::vector<std::pair<double, double>> Sample (const Profile& profile, size_t perSegment);

// An anchor on the curve at `station`, its handle along the curve there. Its index,
// or the existing anchor's when one is there already.
size_t Insert (Profile& profile, double station);
// An interior anchor removed; the two ends stay. False for an end.
bool Remove (Profile& profile, size_t index);
// An anchor moved: an end keeps its station, an interior one stays between its
// neighbours.
void MoveAnchor (Profile& profile, size_t index, double station, double z);
// A handle's tip moved to (station, z): `out` the handle toward the next anchor,
// else the mirrored one toward the previous.
void MoveHandle (Profile& profile, size_t index, double station, double z, bool out);
// Where a handle's tip is drawn: clamped as the curve uses it.
std::pair<double, double> HandleTip (const Profile& profile, size_t index, bool out);

constexpr char kPropertyKey[] = "analysis.pathProfile";
metadata::Property Encode (const Profile& profile);
// False with `error` when the property is not a profile: fewer than two anchors,
// a non-finite number, or stations not increasing.
bool Decode (const metadata::Property& property, Profile& profile, std::string& error);

} // namespace geomsrv::archviz::analysisprofile

#endif
