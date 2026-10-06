#ifndef EVP_ARCHVIZ_MASSINGAREACALCULATIONS_HPP
#define EVP_ARCHVIZ_MASSINGAREACALCULATIONS_HPP

#include "ArchViz/HudShell.hpp"

namespace geomsrv::archviz::massingareas {
struct Coefficients {
    double grossFactor = 0.78, sellableFactor = 0.71;
    double unitGrossArea = 50, parkingAreaPerUnit = 30; // m2 per estimated unit
};
struct Areas {
    double total = 0, gross = 0, sellable = 0, units = 0, parking = 0;
};
struct NumberEdit {
    Coefficients before;
    std::string key;
    double number = 0, min = 0, max = 1;
};
bool Valid (const Coefficients& coefficients);
bool Assign (Coefficients& coefficients, const std::string& key, double number);
bool Same (const Coefficients& a, const Coefficients& b);
Areas Calculate (double total, const Coefficients& coefficients);
// The same derived figures on Stats and Selection, using each page's own area basis.
std::vector<hudshell::Figure> Figures (double total, const Coefficients& coefficients, bool building = false);
} // namespace geomsrv::archviz::massingareas
#endif
