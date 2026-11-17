#ifndef EVP_ARCHVIZ_HUDLENGTHFIELD_HPP
#define EVP_ARCHVIZ_HUDLENGTHFIELD_HPP

// #109 / LD-09: one definition per length field of the editor's panels. The session and the
// document keep metres; the field shows, drags and reads the project's length unit
// (EditDisplayUnits.hpp). Bounds, drag speed and precision are given in metres once, here, and
// converted for the unit shown, so a field reads "1250 mm" in a millimetre project and "1.25 m"
// in a metre one without each panel spelling its own format string.
#include "Edit/EditDisplayUnits.hpp"
#include <string>

namespace geomsrv::archviz::edithud {

struct LengthField {
    const char* label = "";
    double low = 0.0, high = 0.0; // metres; high <= low: unbounded (drag and input only)
    double speed = 0.05;          // metres a drag moves per pixel
    int decimals = 2;             // what the field shows in metres; finer units need fewer
};

// The field as one unit shows it: bounds and speed in that unit, and its printf format with the
// unit's symbol ("%.0f mm"). Pure, so the conversion is tested without drawing.
struct ShownLengthField {
    double low = 0.0, high = 0.0, speed = 0.0;
    int decimals = 0;
    std::string format;
};
ShownLengthField ShowLength (const edit::DisplayUnits& units, const LengthField& field);
// "Contour interval" -> "Contour interval (mm)": a label that names the unit above its control.
std::string WithLengthUnit (const edit::DisplayUnits& units, const char* label);

// ImGui controls. Each changes `metres` and returns true when the user changed it; the control is
// the last item drawn, so IsItemActive / IsItemDeactivated read it as before.
bool LengthDrag (const LengthField& field, double& metres);
bool LengthSlider (const LengthField& field, double& metres);
// Typed: steps in metres (0: no step buttons); ImGui input flags, e.g. EnterReturnsTrue.
bool LengthInput (const LengthField& field, double& metres, double step = 0.0, double fastStep = 0.0, int flags = 0);

} // namespace geomsrv::archviz::edithud
#endif
