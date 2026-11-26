#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/PlantingColumns.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "NodeGraph/Json.hpp"
#include "Python/ApiDispatcher.hpp"
#include "Python/MainThreadGate.hpp"

#include <cstdio>
#include <mutex>

namespace geomsrv::plantingcolumns {

namespace {

namespace js = evp::nodegraph::json;

std::mutex s_mutex;
Status s_status;

// Every number as a real: a whole number written without a fraction would not
// reach the command as a double.
std::string Number (double v)
{
    char text[40];
    std::snprintf (text, sizeof (text), "%.6f", v);
    return text;
}

std::string Array (const std::vector<Column>& columns, double Column::* field)
{
    std::string out = "[";
    for (size_t i = 0; i < columns.size (); ++i)
        out += (i ? "," : "") + Number (columns[i].*field);
    return out + "]";
}

std::string Params (const std::vector<Column>& columns, double diameter)
{
    return "{\"x\":" + Array (columns, &Column::x) + ",\"y\":" + Array (columns, &Column::y) +
           ",\"bases\":" + Array (columns, &Column::base) + ",\"heights\":" + Array (columns, &Column::height) +
           ",\"shape\":\"circular\",\"diameter\":" + Number (diameter) + "}";
}

void Finish (bool ok, size_t created, const std::string& message)
{
    std::lock_guard<std::mutex> lock (s_mutex);
    s_status.busy = false;
    s_status.ok = ok;
    s_status.created = created;
    s_status.message = message;
    ++s_status.serial;
}

// MAIN THREAD.
void Create (const std::string& params, size_t wanted)
{
    const GS::UniString response = evp::DispatchApiCall (GS::UniString ("Tapioca.CreateColumn", CC_UTF8),
                                                         GS::UniString (params.c_str (), CC_UTF8));
    const std::string text = response.ToCStr (0, MaxUSize, CC_UTF8).Get ();
    const auto parsed = js::Parse (text);
    bool ok = false;
    const auto* flag = parsed.value.Find ("ok");
    const auto* data = parsed.value.Find ("data");
    double count = 0;
    if (parsed.ok && flag && flag->AsBool (ok) && ok && data) {
        if (const auto* c = data->Find ("count"))
            c->AsDouble (count);
        const size_t created = size_t (count);
        char message[160];
        std::snprintf (message, sizeof (message),
                       created == wanted ? "%zu round Columns created. One Undo in Archicad removes them."
                                         : "%zu of %zu round Columns created; see the add-on log for the rest.",
                       created, wanted);
        archviz::ArchVizLog (std::string ("planting columns: ") + message);
        Finish (created > 0, created, message);
        return;
    }
    std::string why;
    if (const auto* error = parsed.value.Find ("error"))
        if (!error->AsString (why))
            if (const auto* m = error->Find ("message"))
                m->AsString (why);
    archviz::ArchVizLog ("planting columns: not created: " + (why.empty () ? text : why));
    Finish (false, 0, "The Columns were not created: " + (why.empty () ? text : why));
}

} // namespace

bool Request (std::vector<Column> columns, double diameter, std::string& error)
{
    if (columns.empty ()) {
        error = "there is no placed plant";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock (s_mutex);
        if (s_status.busy) {
            error = "a create is already running";
            return false;
        }
        s_status.busy = true;
        s_status.message = "Creating the Columns...";
    }
    const std::string params = Params (columns, diameter);
    const size_t wanted = columns.size ();
    GS::UniString gateError;
    if (!evp::MainThreadGate::Get ().Post ([params, wanted] { Create (params, wanted); }, gateError)) {
        error = "not queued: " + std::string (gateError.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        Finish (false, 0, "The Columns were not created: " + error);
        return false;
    }
    return true;
}

Status Current ()
{
    std::lock_guard<std::mutex> lock (s_mutex);
    return s_status;
}

} // namespace geomsrv::plantingcolumns
