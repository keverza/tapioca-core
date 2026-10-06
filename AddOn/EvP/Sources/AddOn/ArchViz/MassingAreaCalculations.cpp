#include "ArchViz/MassingAreaCalculations.hpp"
#include <cmath>
#include <cstdio>

namespace geomsrv::archviz::massingareas {
bool Valid (const Coefficients& c)
{
    return std::isfinite (c.grossFactor) && c.grossFactor >= 0 && c.grossFactor <= 1 &&
           std::isfinite (c.sellableFactor) && c.sellableFactor >= 0 && c.sellableFactor <= 1 &&
           std::isfinite (c.unitGrossArea) && c.unitGrossArea >= 0.01 && c.unitGrossArea <= 1000000 &&
           std::isfinite (c.parkingAreaPerUnit) && c.parkingAreaPerUnit >= 0 && c.parkingAreaPerUnit <= 1000000;
}
bool Same (const Coefficients& a, const Coefficients& b)
{
    return a.grossFactor == b.grossFactor && a.sellableFactor == b.sellableFactor &&
           a.unitGrossArea == b.unitGrossArea && a.parkingAreaPerUnit == b.parkingAreaPerUnit;
}
bool Assign (Coefficients& coefficients, const std::string& key, double number)
{
    auto wanted = coefficients;
    if (key == "Gross area factor")
        wanted.grossFactor = number;
    else if (key == "Sellable area factor")
        wanted.sellableFactor = number;
    else if (key == "Gross m2 per unit")
        wanted.unitGrossArea = number;
    else if (key == "Parking m2 per unit")
        wanted.parkingAreaPerUnit = number;
    else
        return false;
    if (!Valid (wanted))
        return false;
    coefficients = wanted;
    return true;
}
Areas Calculate (double total, const Coefficients& c)
{
    if (!Valid (c) || !std::isfinite (total) || total < 0)
        return {};
    Areas out;
    out.total = total;
    out.gross = total * c.grossFactor;
    out.sellable = total * c.sellableFactor;
    out.units = out.gross / c.unitGrossArea;
    out.parking = out.units * c.parkingAreaPerUnit;
    return out;
}
std::vector<hudshell::Figure> Figures (double total, const Coefficients& c, bool building)
{
    const auto areas = Calculate (total, c);
    const auto text = [] (double value, const char* suffix) {
        char buffer[96];
        std::snprintf (buffer, sizeof (buffer), "%.2f%s", value, suffix);
        return std::string (buffer);
    };
    const auto factor = [] (double value) {
        char buffer[32];
        std::snprintf (buffer, sizeof (buffer), "%.4g", value);
        return std::string (buffer);
    };
    return { { building ? "Total building area" : "Total floor area", text (areas.total, " m2") },
             { std::string (building ? "Gross building area" : "Gross area") + " (total x " + factor (c.grossFactor) +
                   ")",
               text (areas.gross, " m2") },
             { "Sellable area (total x " + factor (c.sellableFactor) + ")", text (areas.sellable, " m2") },
             { "Unit count (gross / " + factor (c.unitGrossArea) + ")", text (areas.units, "") },
             { "Parking area (units x " + factor (c.parkingAreaPerUnit) + ")", text (areas.parking, " m2") } };
}
} // namespace geomsrv::archviz::massingareas
