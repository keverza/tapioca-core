#include "ArchViz/HudLengthField.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::edithud {

ShownLengthField ShowLength (const edit::DisplayUnits& units, const LengthField& field)
{
    ShownLengthField shown;
    const double scale = edit::MetresToLength (units, 1.0);
    shown.low = field.low * scale;
    shown.high = field.high * scale;
    shown.speed = field.speed * scale;
    // The field's own precision, carried into the unit (3 decimals of a metre are 0 of a
    // millimetre), and never coarser than the project asks for.
    const int carried = field.decimals - int (std::lround (std::log10 (scale)));
    shown.decimals = std::clamp (std::max (carried, units.fromProject ? units.lengthDecimals : 0), 0, 6);
    shown.format = "%." + std::to_string (shown.decimals) + "f " + edit::LengthSymbol (units);
    return shown;
}

std::string WithLengthUnit (const edit::DisplayUnits& units, const char* label)
{
    return std::string (label) + " (" + edit::LengthSymbol (units) + ")";
}

namespace {
bool Bounded (const ShownLengthField& f)
{
    return f.high > f.low;
}

bool Store (const edit::DisplayUnits& units, const ShownLengthField& f, double shown, double& metres)
{
    if (!std::isfinite (shown))
        return false;
    if (Bounded (f))
        shown = std::clamp (shown, f.low, f.high);
    const double next = edit::LengthToMetres (units, shown);
    if (next == metres)
        return false;
    metres = next;
    return true;
}
} // namespace

bool LengthDrag (const LengthField& field, double& metres)
{
    const auto units = edit::CurrentDisplayUnits ();
    const auto f = ShowLength (units, field);
    double shown = edit::MetresToLength (units, metres);
    const bool bounded = Bounded (f);
    if (!ImGui::DragScalar (field.label, ImGuiDataType_Double, &shown, float (f.speed), bounded ? &f.low : nullptr,
                            bounded ? &f.high : nullptr, f.format.c_str (),
                            bounded ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None))
        return false;
    return Store (units, f, shown, metres);
}

bool LengthSlider (const LengthField& field, double& metres)
{
    const auto units = edit::CurrentDisplayUnits ();
    const auto f = ShowLength (units, field);
    double shown = edit::MetresToLength (units, metres);
    if (!ImGui::SliderScalar (field.label, ImGuiDataType_Double, &shown, &f.low, &f.high, f.format.c_str ()))
        return false;
    return Store (units, f, shown, metres);
}

bool LengthInput (const LengthField& field, double& metres, double step, double fastStep, int flags)
{
    const auto units = edit::CurrentDisplayUnits ();
    const auto f = ShowLength (units, field);
    double shown = edit::MetresToLength (units, metres);
    const double scale = edit::MetresToLength (units, 1.0);
    const double steps[2] = { step * scale, fastStep * scale };
    if (!ImGui::InputScalar (field.label, ImGuiDataType_Double, &shown, step > 0.0 ? &steps[0] : nullptr,
                             fastStep > 0.0 ? &steps[1] : nullptr, f.format.c_str (), flags))
        return false;
    return Store (units, f, shown, metres);
}

} // namespace geomsrv::archviz::edithud
