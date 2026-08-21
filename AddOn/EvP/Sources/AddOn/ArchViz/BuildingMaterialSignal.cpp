#include "ArchViz/BuildingMaterialSignal.hpp"

namespace geomsrv {
namespace archviz {

const std::map<int, Substance>& StockPriorities ()
{
    // ⚠️ EVERY ENTRY IS A MEASURED STOCK VALUE, not a chosen one. Read off
    // Archicad's own default building materials and confirmed identical in two
    // unrelated projects (BuildingMaterialDump, 2026-08-21): 58 of the 59 names
    // present in both carry the same priority.
    //
    // ⚠️ THE SAME SUBSTANCE APPEARS AT SEVERAL PRIORITIES because the priority
    // encodes ROLE, not material. Timber is structural at 910, roof at 810 and
    // floor at 220. Do not attempt to compress this into ranges.
    //
    // Substances Archicad also ships and this table deliberately OMITS -- brick
    // (720/640/540), stone (710/630), masonry block (730/520), plaster
    // (620/210), gypsum board (330/320), insulation (440/430/420/410),
    // membrane (830/470), tile (840/250/230) and the GENERIC family. They are
    // omitted because `Substance` has no member for them, and inventing one
    // without a renderer preset behind it would be a name, not a signal.
    static const std::map<int, Substance> kStock = {
        // Steel - Stainless / Structural / Steel / Aluminium / Iron, Titanium Zinc
        { 970, Substance::Metal },
        { 960, Substance::Metal },
        { 950, Substance::Metal },
        { 940, Substance::Metal },
        { 930, Substance::Metal },
        { 850, Substance::Metal },

        // Reinforced Concrete - Prefab / Structural, Concrete - Structural,
        // Concrete Block - Structural / Filler, Concrete
        { 920, Substance::Concrete },
        { 760, Substance::Concrete },
        { 750, Substance::Concrete },
        { 740, Substance::Concrete },
        { 530, Substance::Concrete },
        { 510, Substance::Concrete },

        // Timber - Structural / Roof / Floor, Plywood, Fiberboard
        { 910, Substance::Wood },
        { 810, Substance::Wood },
        { 350, Substance::Wood },
        { 340, Substance::Wood },
        { 220, Substance::Wood },

        // Glass
        { 550, Substance::Glass },

        // Insulation - Plastic Hard / Soft, Plastic - Solid
        { 460, Substance::Plastic },
        { 450, Substance::Plastic },
        { 240, Substance::Plastic },

        // Soil, Gravel, Sand
        { 140, Substance::Earth },
        { 130, Substance::Earth },
        { 120, Substance::Earth },
    };
    return kStock;
}

namespace {

Substance FromPriority (int priority)
{
    const std::map<int, Substance>& stock = StockPriorities ();
    const auto it = stock.find (priority);
    return it == stock.end () ? Substance::Unknown : it->second;
}

} // namespace

std::map<std::string, Substance> DeriveFillSubstances (const std::vector<BuildingMaterialRow>& rows)
{
    // fill -> the substances its stock-priority members vote for.
    std::map<std::string, std::vector<Substance>> votes;
    for (const BuildingMaterialRow& row : rows) {
        if (row.cutFillId.empty ())
            continue;
        const Substance substance = FromPriority (row.connectionPriority);
        if (substance != Substance::Unknown)
            votes[row.cutFillId].push_back (substance);
    }

    std::map<std::string, Substance> fills;
    for (const auto& entry : votes) {
        const std::vector<Substance>& cast = entry.second;

        // ⚠️ TWO WITNESSES, AND THEY MUST AGREE. One witness is circular -- it
        // is usually the material being classified, confirming itself. A
        // disagreement means the fill is shared across substances, which the
        // measured templates do: one fill carries air space, iron AND zinc, and
        // another carries plastic, a vapour barrier AND aluminium. Either way
        // the fill gets no opinion rather than a guessed one.
        if (cast.size () < 2)
            continue;
        bool unanimous = true;
        for (const Substance s : cast) {
            if (s != cast.front ()) {
                unanimous = false;
                break;
            }
        }
        if (unanimous)
            fills[entry.first] = cast.front ();
    }
    return fills;
}

SubstanceVerdict ClassifyBuildingMaterial (const BuildingMaterialRow& row,
                                           const std::map<std::string, Substance>& fillSubstances)
{
    const Substance byPriority = FromPriority (row.connectionPriority);

    Substance byFill = Substance::Unknown;
    if (!row.cutFillId.empty ()) {
        const auto it = fillSubstances.find (row.cutFillId);
        if (it != fillSubstances.end ())
            byFill = it->second;
    }

    // Both signals, agreeing. Measured 100% correct over 157 materials in two
    // projects -- including three user-authored ones whose names were not read.
    if (byPriority != Substance::Unknown && byPriority == byFill)
        return { byPriority, 0.90f };

    // ⚠️ THE FILL IS NEVER BELIEVED ON ITS OWN. Measured, fill-only is what
    // called "GENERIC - ENVIRONMENT" glass: the default fill of a template is
    // shared by everything nobody gave a real hatch to, and glass happens to sit
    // in it. There is no low-confidence version of that verdict worth having.
    if (byPriority == Substance::Unknown)
        return { Substance::Unknown, 0.0f };

    // A known stock priority the fill will not confirm. Reported weakly rather
    // than promoted: this is the tier that calls "## CONCEPT - MARBLE" wood,
    // because the user authored it at Plywood's priority.
    if (byFill == Substance::Unknown)
        return { byPriority, 0.55f };

    // The two signals contradict each other outright. That is information --
    // it is exactly how marble is caught when its fill IS known -- and the one
    // thing it must not produce is a verdict.
    return { Substance::Unknown, 0.0f };
}

const char* SubstanceName (Substance substance)
{
    switch (substance) {
        case Substance::Earth:
            return "earth";
        case Substance::Concrete:
            return "concrete";
        case Substance::Metal:
            return "metal";
        case Substance::Plastic:
            return "plastic";
        case Substance::Glass:
            return "glass";
        case Substance::Wood:
            return "wood";
        case Substance::Unknown:
            break;
    }
    // No `default:` -- adding a substance should make the compiler point here.
    return "unknown";
}

} // namespace archviz
} // namespace geomsrv
