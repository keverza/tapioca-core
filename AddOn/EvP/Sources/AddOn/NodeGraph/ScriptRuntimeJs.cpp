// The JavaScript half of the script node family, on QuickJS.
//
// ⚠️ WHY AN EMBEDDED ENGINE RATHER THAN THE WEBVIEW THAT IS ALREADY THERE. The
// browser could run this JavaScript today for free. It was not used, because the
// graph runtime's founding rule is that C++ owns evaluation - which is what makes
// a headless run, a test and a rebuild-free CI possible. A node family that could
// only be evaluated while a palette happened to be open would be the one node in
// the catalog whose results depended on whether anybody was looking at it.
//
// QuickJS costs a pinned reference entry and a NOTICE line. What it buys is that
// every test below runs in the offline suite, with no Archicad and no rebuild.
//
// ⚠️ THE INTERPRETER IS PER-CALL, NOT PER-PROCESS, AND THAT IS DELIBERATE. A
// shared context would let one node's globals leak into the next node's script -
// which reads as a node that works until someone reorders the graph. A fresh
// JSRuntime per evaluation costs microseconds next to reading the file, and it
// makes the worker pool's parallelism free: there is no shared interpreter state
// to serialise on.

#include "NodeGraph/ScriptRuntime.hpp"

#include "NodeGraph/Value.hpp"

// quickjs-ng, not Bellard's quickjs: it is the fork that builds under MSVC and
// carries a CMake graph. The two have diverged in small API details - JS_IsArray
// takes a JSValue here and a (ctx, value) pair there - so the catalog entry pins
// a version rather than a branch.
#include <quickjs.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace evp::nodegraph {
namespace {

// What the interrupt handler needs to decide whether to stop the script. QuickJS
// calls it every so many bytecode instructions, which is the only place a
// non-returning script can be stopped from - a watchdog thread has nothing to
// interrupt, because the script is not blocked, it is running.
struct InterruptState {
    std::chrono::steady_clock::time_point deadline;
    const CancellationToken* cancellation = nullptr;
    bool timedOut = false;
    bool cancelled = false;
};

int OnInterrupt (JSRuntime*, void* opaque)
{
    InterruptState& state = *static_cast<InterruptState*> (opaque);
    if (state.cancellation != nullptr && state.cancellation->IsCancelled ()) {
        state.cancelled = true;
        return 1;
    }
    if (std::chrono::steady_clock::now () >= state.deadline) {
        state.timedOut = true;
        return 1;
    }
    return 0;
}

std::string Utf8 (JSContext* context, JSValueConst value)
{
    size_t length = 0;
    const char* text = JS_ToCStringLen (context, &length, value);
    if (text == nullptr)
        return {};
    std::string result (text, length);
    JS_FreeCString (context, text);
    return result;
}

// ---------------------------------------------------------------------------
// Value marshalling.
//
// ⚠️ THE MAPPING IS NARROW ON PURPOSE, AND EVERY GAP IN IT IS A REPORTED ERROR
// RATHER THAN A SILENT COERCION. A mesh handed to a script becomes an object the
// script can read and NOT one it can rebuild: reconstructing geometry from
// whatever a script left in a variable is how a graph acquires meshes with three
// vertices and no normals, failing far downstream in the renderer. A script that
// wants to make geometry uses the geometry nodes, which is where that knowledge
// already lives.

JSValue ToJs (JSContext* context, const Value& value);
JSValue ToJs (JSContext* context, const Argument& value);

JSValue Point3ToJs (JSContext* context, const Point3& point)
{
    JSValue object = JS_NewObject (context);
    JS_SetPropertyStr (context, object, "x", JS_NewFloat64 (context, point.x));
    JS_SetPropertyStr (context, object, "y", JS_NewFloat64 (context, point.y));
    JS_SetPropertyStr (context, object, "z", JS_NewFloat64 (context, point.z));
    return object;
}

JSValue PointsToJs (JSContext* context, const std::vector<Point3>& points)
{
    JSValue array = JS_NewArray (context);
    uint32_t index = 0;
    for (const Point3& point : points)
        JS_SetPropertyUint32 (context, array, index++, Point3ToJs (context, point));
    return array;
}

JSValue ToJs (JSContext* context, const Value& value)
{
    switch (value.Type ()) {
        case ValueType::Absent:
            return JS_NULL;
        case ValueType::Bool:
            return JS_NewBool (context, std::get<bool> (value.DataValue ()) ? 1 : 0);
        case ValueType::Integer:
            return JS_NewInt64 (context, std::get<int64_t> (value.DataValue ()));
        case ValueType::Double:
            return JS_NewFloat64 (context, std::get<double> (value.DataValue ()));
        case ValueType::String:
            return JS_NewString (context, std::get<std::string> (value.DataValue ()).c_str ());
        case ValueType::Point3:
            return Point3ToJs (context, std::get<Point3> (value.DataValue ()));
        case ValueType::Polyline:
            return PointsToJs (context, std::get<Polyline> (value.DataValue ()).points);
        case ValueType::Polygon:
            return PointsToJs (context, std::get<Polygon> (value.DataValue ()).points);
        case ValueType::ArchicadElementRef: {
            // A GUID string in an object, not a bare string: an element is a
            // reference to something in the model, and a script that could pass a
            // string where an element was wanted would be able to fabricate one.
            JSValue object = JS_NewObject (context);
            JS_SetPropertyStr (context, object, "elementGuid",
                               JS_NewString (context, std::get<ArchicadElementRef> (value.DataValue ()).guid.c_str ()));
            return object;
        }
        case ValueType::Mesh: {
            // Readable, not reconstructable - see the note above.
            const Value::ImmutableMesh& mesh = std::get<Value::ImmutableMesh> (value.DataValue ());
            JSValue object = JS_NewObject (context);
            JS_SetPropertyStr (context, object, "isMesh", JS_TRUE);
            JS_SetPropertyStr (context, object, "vertexCount",
                               JS_NewInt64 (context, mesh ? static_cast<int64_t> (mesh->VertexCount ()) : 0));
            JS_SetPropertyStr (context, object, "triangleCount",
                               JS_NewInt64 (context, mesh ? static_cast<int64_t> (mesh->TriangleCount ()) : 0));
            return object;
        }
        case ValueType::List:
            // Unreachable: a Value can no longer carry a List - see the
            // Argument overload, which handles the branch before an item is
            // ever passed here.
            return JS_NULL;
    }
    return JS_NULL;
}

JSValue ToJs (JSContext* context, const Argument& value)
{
    if (value.Type () != ValueType::List)
        return ToJs (context, value.AsValue ());
    JSValue array = JS_NewArray (context);
    uint32_t index = 0;
    for (const Value& item : value.Items ())
        JS_SetPropertyUint32 (context, array, index++, ToJs (context, item));
    return array;
}

bool FromJs (JSContext* context, JSValueConst value, ValueType expected, Value& out, std::string& error);
bool FromJs (JSContext* context, JSValueConst value, ValueType expected, Argument& out, std::string& error);

bool PointFromJs (JSContext* context, JSValueConst value, Point3& point, std::string& error)
{
    if (!JS_IsObject (value)) {
        error = "expected a point like { x: 0, y: 0, z: 0 }";
        return false;
    }
    double components[3] = { 0.0, 0.0, 0.0 };
    const char* names[3] = { "x", "y", "z" };
    for (size_t axis = 0; axis < 3; ++axis) {
        JSValue member = JS_GetPropertyStr (context, value, names[axis]);
        const int converted = JS_ToFloat64 (context, &components[axis], member);
        JS_FreeValue (context, member);
        if (converted < 0) {
            error = std::string ("a point's ") + names[axis] + " is not a number";
            return false;
        }
    }
    point = Point3 { components[0], components[1], components[2] };
    return true;
}

bool PointsFromJs (JSContext* context, JSValueConst value, std::vector<Point3>& points, std::string& error)
{
    if (!JS_IsArray (value)) {
        error = "expected an array of points";
        return false;
    }
    JSValue lengthValue = JS_GetPropertyStr (context, value, "length");
    uint32_t length = 0;
    JS_ToUint32 (context, &length, lengthValue);
    JS_FreeValue (context, lengthValue);
    for (uint32_t index = 0; index < length; ++index) {
        JSValue item = JS_GetPropertyUint32 (context, value, index);
        Point3 point;
        const bool ok = PointFromJs (context, item, point, error);
        JS_FreeValue (context, item);
        if (!ok)
            return false;
        points.push_back (point);
    }
    return true;
}

bool FromJs (JSContext* context, JSValueConst value, ValueType expected, Value& out, std::string& error)
{
    switch (expected) {
        case ValueType::Bool:
            out = Value (JS_ToBool (context, value) != 0);
            return true;
        case ValueType::Integer: {
            int64_t number = 0;
            if (JS_ToInt64 (context, &number, value) < 0) {
                error = "expected a whole number";
                return false;
            }
            out = Value (number);
            return true;
        }
        case ValueType::Double: {
            double number = 0.0;
            if (JS_ToFloat64 (context, &number, value) < 0) {
                error = "expected a number";
                return false;
            }
            out = Value (number);
            return true;
        }
        case ValueType::String:
            out = Value (Utf8 (context, value));
            return true;
        case ValueType::Point3: {
            Point3 point;
            if (!PointFromJs (context, value, point, error))
                return false;
            out = Value (point);
            return true;
        }
        case ValueType::Polyline: {
            Polyline polyline;
            if (!PointsFromJs (context, value, polyline.points, error))
                return false;
            out = Value (std::move (polyline));
            return true;
        }
        case ValueType::Polygon: {
            Polygon polygon;
            if (!PointsFromJs (context, value, polygon.points, error))
                return false;
            out = Value (std::move (polygon));
            return true;
        }
        case ValueType::List:
            // Unreachable: a scalar Value can no longer carry a List - see the
            // Argument overload.
            error = "a scalar value cannot carry a list";
            return false;
        case ValueType::ArchicadElementRef: {
            JSValue guid = JS_GetPropertyStr (context, value, "elementGuid");
            // ⚠️ THE TYPE IS CHECKED BEFORE THE CONVERSION, and skipping that let a
            // script fabricate an element out of `{}`. JS_ToCStringLen does not
            // fail on a missing property: it stringifies `undefined` into the
            // seven-character word "undefined", which is not empty - so an
            // emptiness test passed it through as a perfectly good element
            // reference pointing at nothing. Caught by
            // RefusesToLetAScriptFabricateGeometryOrAnElement.
            const bool isString = JS_IsString (guid);
            const std::string text = isString ? Utf8 (context, guid) : std::string {};
            JS_FreeValue (context, guid);
            if (!isString || text.empty ()) {
                error = "expected an element, as handed to the script - a script cannot make one";
                return false;
            }
            out = Value (ArchicadElementRef { text });
            return true;
        }
        case ValueType::Mesh:
            // Refused rather than half-implemented: see the marshalling note.
            error = "a script cannot produce a mesh; use the geometry nodes and wire the result";
            return false;
        case ValueType::Absent:
            // An `any` output. Nothing can be checked and nothing should be
            // invented, so it travels as text - which is what a Panel would show
            // anyway, and is honest about having lost the type.
            out = Value (Utf8 (context, value));
            return true;
    }
    error = "unsupported type";
    return false;
}

bool FromJs (JSContext* context, JSValueConst value, ValueType expected, Argument& out, std::string& error)
{
    if (expected != ValueType::List) {
        Value scalar;
        if (!FromJs (context, value, expected, scalar, error))
            return false;
        out = Argument (std::move (scalar));
        return true;
    }

    if (!JS_IsArray (value)) {
        error = "expected an array";
        return false;
    }
    JSValue lengthValue = JS_GetPropertyStr (context, value, "length");
    uint32_t length = 0;
    JS_ToUint32 (context, &length, lengthValue);
    JS_FreeValue (context, lengthValue);
    std::vector<Value> items;
    for (uint32_t index = 0; index < length; ++index) {
        JSValue item = JS_GetPropertyUint32 (context, value, index);
        Value decoded;
        // A list's members are read as numbers, which is what every list in
        // the catalog carries today. A heterogeneous list would need a
        // per-item type the header has no way to state, so it is refused
        // rather than guessed.
        const bool ok = FromJs (context, item, ValueType::Double, decoded, error);
        JS_FreeValue (context, item);
        if (!ok)
            return false;
        items.push_back (std::move (decoded));
    }
    out = Argument::FromItems (std::move (items));
    return true;
}

// `console.log` and friends. Not a convenience: a script node runs on a worker
// thread inside Archicad with no console attached, so without this a print
// statement goes nowhere and printf debugging - the way people actually debug a
// script - silently does not work.
JSValue OnConsoleLog (JSContext* context, JSValueConst, int argc, JSValueConst* argv)
{
    auto* log = static_cast<std::vector<std::string>*> (JS_GetContextOpaque (context));
    if (log == nullptr)
        return JS_UNDEFINED;
    std::string line;
    for (int index = 0; index < argc; ++index) {
        if (index > 0)
            line += " ";
        line += Utf8 (context, argv[index]);
    }
    // Bounded: a script logging in a loop must not turn into unbounded memory in
    // the add-on's process before the time budget stops it.
    constexpr size_t kMaxLogLines = 200;
    if (log->size () < kMaxLogLines)
        log->push_back (std::move (line));
    else if (log->size () == kMaxLogLines)
        log->push_back ("... further output suppressed");
    return JS_UNDEFINED;
}

class QuickJsRuntime final : public IScriptRuntime {
  public:
    ScriptRunResult Run (const ScriptRunRequest& request) override
    {
        ScriptRunResult result;

        JSRuntime* runtime = JS_NewRuntime ();
        if (runtime == nullptr) {
            result.error = "the JavaScript engine could not start";
            return result;
        }
        // A script's own memory ceiling, separate from the time budget: an
        // allocation loop reaches this long before it reaches the machine's, and
        // failing one script is better than failing Archicad.
        JS_SetMemoryLimit (runtime, 64 * 1024 * 1024);
        JS_SetMaxStackSize (runtime, 1024 * 1024);

        InterruptState interrupt;
        interrupt.deadline =
            std::chrono::steady_clock::now () + std::chrono::milliseconds (static_cast<int64_t> (request.timeBudgetMs));
        interrupt.cancellation = &request.cancellation;
        JS_SetInterruptHandler (runtime, OnInterrupt, &interrupt);

        JSContext* context = JS_NewContext (runtime);
        if (context == nullptr) {
            JS_FreeRuntime (runtime);
            result.error = "the JavaScript engine could not start";
            return result;
        }
        JS_SetContextOpaque (context, &result.log);

        JSValue global = JS_GetGlobalObject (context);
        JSValue console = JS_NewObject (context);
        JS_SetPropertyStr (context, console, "log", JS_NewCFunction (context, OnConsoleLog, "log", 1));
        JS_SetPropertyStr (context, global, "console", console);

        for (const auto& [portId, value] : request.inputs)
            JS_SetPropertyStr (context, global, portId.c_str (), ToJs (context, value));

        // Evaluated as a plain script, not a module: a module's top-level
        // bindings are not properties of the global object, so the outputs could
        // not be read back - and a script node's contract is "assign to the names
        // your header declared".
        const JSValue evaluated =
            JS_Eval (context, request.source.c_str (), request.source.size (),
                     request.path.empty () ? "<script>" : request.path.c_str (), JS_EVAL_TYPE_GLOBAL);

        if (JS_IsException (evaluated)) {
            if (interrupt.cancelled)
                result.error = "the run was cancelled";
            else if (interrupt.timedOut)
                result.error = "the script ran longer than its time budget and was stopped";
            else {
                const JSValue exception = JS_GetException (context);
                result.error = Utf8 (context, exception);
                JSValue stack = JS_GetPropertyStr (context, exception, "stack");
                if (!JS_IsUndefined (stack)) {
                    const std::string trace = Utf8 (context, stack);
                    if (!trace.empty ())
                        result.log.push_back (trace);
                }
                JS_FreeValue (context, stack);
                JS_FreeValue (context, exception);
                if (result.error.empty ())
                    result.error = "the script failed";
            }
        }
        else {
            result.ok = true;
            for (const PortSchema& output : request.outputs) {
                JSValue produced = JS_GetPropertyStr (context, global, output.id.c_str ());
                if (JS_IsUndefined (produced)) {
                    // Named, always. "The script did not set 'area'" is something
                    // the author can fix by looking at one line.
                    result.error = "the script did not set '" + output.id + "'";
                    result.ok = false;
                    JS_FreeValue (context, produced);
                    break;
                }
                Argument decoded;
                std::string error;
                const bool converted = FromJs (context, produced, output.valueType, decoded, error);
                JS_FreeValue (context, produced);
                if (!converted) {
                    result.error = "'" + output.id + "': " + error;
                    result.ok = false;
                    break;
                }
                result.outputs.insert_or_assign (output.id, std::move (decoded));
            }
            if (!result.ok)
                result.outputs.clear ();
        }

        JS_FreeValue (context, evaluated);
        JS_FreeValue (context, global);
        JS_FreeContext (context);
        JS_FreeRuntime (runtime);
        return result;
    }
};

QuickJsRuntime gJavaScriptRuntime;

} // namespace

void InstallJavaScriptRuntime ()
{
    SetActiveScriptRuntime (ScriptLanguage::JavaScript, &gJavaScriptRuntime);
}

} // namespace evp::nodegraph
