#ifndef EVP_NATIVECOMMANDS_PLANTINGCOLUMNS_HPP
#define EVP_NATIVECOMMANDS_PLANTINGCOLUMNS_HPP

// NativeCommands/PlantingColumns -- a placed planting scheme (#278) written to
// Archicad as round Columns: one per plant, on request only.
//
// The HUD asks from the render thread; the create runs on the main thread
// through the command dispatcher (Tapioca.CreateColumn, one call), which owns
// the undo step, so one Undo in Archicad removes every column. The UI
// playground stubs these two calls.

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::plantingcolumns {

struct Column {
    double x = 0, y = 0; // plan position, m
    double base = 0;     // world z of the column's bottom, m
    double height = 0;   // m
};

struct Status {
    uint64_t serial = 0; // moves on every finished request
    bool busy = false;
    bool ok = false;
    size_t created = 0;
    std::string message; // what the panel says
};

// Queues one create of `columns`, round, `diameter` m wide, with the Column
// tool's other defaults (layer, material, story). False, with `error`, when a
// create is already running or the main thread cannot take it.
bool Request (std::vector<Column> columns, double diameter, std::string& error);

Status Current ();

} // namespace geomsrv::plantingcolumns

#endif
