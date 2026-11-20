#ifndef EVP_EDIT_EDITTERRAINBAKEDIVIDE_HPP
#define EVP_EDIT_EDITTERRAINBAKEDIVIDE_HPP

// tapioca-dev #177: contours divided by length that keep clear of each other.
//
// A divided contour is a chain of chords between equally spaced points on the traced line, and a
// chord cuts the corner it spans. On a long step and close levels it can cross the next contour or
// leave through the outline, and the Mesh's triangulation refuses crossing lines. So every piece
// that would cross another line, itself or the outline is halved along its contour, and halved
// again, until it clears; a piece already shorter than the tolerance takes the traced points of its
// stretch instead. The step stays even where the contours lie apart and is shortened only where
// they come close. Only when the traced lines themselves would cross is there no plan.
#include "Edit/EditTypes.hpp"
#include <string>
#include <vector>

namespace geomsrv::edit {

struct DividedContours {
    bool ok = false;
    std::string error;                    // where the lines could not be kept apart
    std::vector<std::vector<Vec3>> lines; // per input line; a ring's first point is not repeated
    size_t shortened = 0;                 // lines with a piece shortened to keep clear
};

// `traced` lines (with `closed` per line) cut into pieces of about `step` metres, their ends kept
// exactly. `outline` is a closed ring they must stay inside. A line keeps at least three pieces
// as a ring and one when open.
DividedContours DivideContours (const std::vector<std::vector<Vec3>>& traced, const std::vector<char>& closed,
                                const std::vector<Vec3>& outline, double step, double tolerance);

} // namespace geomsrv::edit
#endif
