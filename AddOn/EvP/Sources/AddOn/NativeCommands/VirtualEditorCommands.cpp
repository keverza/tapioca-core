#include "APIEnvir.h"
#include "ACAPinc.h"
#include "NativeCommands/VirtualEditorCommands.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/OverlayController.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "Edit/EditPathEdit.hpp"

// Tapioca.VirtualEditor opens, reports and closes the virtual editor; Tapioca.EditorState
// is its read-only diagnostic. Neither writes to the model and neither opens an undo
// scope: the editor's edits live in its own history until a later Accept commits them.
namespace geomsrv {
namespace {

namespace hud = archviz::overlayhud;
namespace eh = archviz::edithud;
namespace el = archviz::editlayers;

std::string Utf8 (const GS::UniString& text)
{
    return text.ToCStr (0, MaxUSize, CC_UTF8).Get ();
}

bool ReadPoints (const GS::ObjectState& item, const GS::String& key, std::vector<edit::Vec3>& out)
{
    GS::Array<GS::ObjectState> points;
    if (!item.Get (key, points))
        return false;
    for (const auto& p : points) {
        edit::Vec3 v;
        if (!ReadFiniteNumber (p, "x", v.x) || !ReadFiniteNumber (p, "y", v.y) || !ReadFiniteNumber (p, "z", v.z))
            return false;
        out.push_back (v);
    }
    return true;
}

std::string Role (const GS::ObjectState& item, const std::string& fallback)
{
    GS::UniString role;
    return item.Get ("role", role) ? Utf8 (role) : fallback;
}

bool Seed (const GS::ObjectState& params, const std::string& role, edit::Document& doc, edit::SnapContext& context,
           std::string& error)
{
    GS::Array<GS::ObjectState> list;
    if (params.Get ("polylines", list))
        for (const auto& item : list) {
            std::vector<edit::Vec3> points;
            bool closed = false;
            item.Get ("closed", closed);
            if (!ReadPoints (item, "points", points)) {
                error = "Polyline points must be finite.";
                return false;
            }
            edit::AddPolyline (doc, points, closed, Role (item, role));
        }
    list.Clear ();
    if (params.Get ("boxes", list))
        for (const auto& item : list) {
            std::vector<edit::Vec3> outline;
            double height = 0.0;
            if (!ReadPoints (item, "outline", outline) || !ReadFiniteNumber (item, "height", height)) {
                error = "Box outlines and heights must be finite.";
                return false;
            }
            edit::AddExtrusion (doc, outline, height, Role (item, role));
        }
    list.Clear ();
    if (params.Get ("paths", list))
        for (const auto& item : list) {
            std::vector<edit::Vec3> points;
            double width = 1.5;
            if (item.Contains ("width"))
                ReadFiniteNumber (item, "width", width);
            if (!ReadPoints (item, "points", points)) {
                error = "Path points must be finite.";
                return false;
            }
            edit::AddPath (doc, points, width, Role (item, role));
        }
    std::vector<edit::Vec3> points;
    if (params.Contains ("snapPoints") && !ReadPoints (params, "snapPoints", points)) {
        error = "Snap points must be finite.";
        return false;
    }
    for (const auto& p : points)
        context.points.push_back ({ p, "model" });
    GS::Array<GS::Array<GS::ObjectState>> segments;
    if (params.Get ("snapSegments", segments))
        for (const auto& pair : segments) {
            edit::Vec3 a, b;
            if (pair.GetSize () != 2 || !ReadFiniteNumber (pair[0], "x", a.x) ||
                !ReadFiniteNumber (pair[0], "y", a.y) || !ReadFiniteNumber (pair[0], "z", a.z) ||
                !ReadFiniteNumber (pair[1], "x", b.x) || !ReadFiniteNumber (pair[1], "y", b.y) ||
                !ReadFiniteNumber (pair[1], "z", b.z)) {
                error = "Snap segments must be pairs of finite points.";
                return false;
            }
            context.segments.push_back ({ a, b, "model" });
        }
    return edit::Validate (doc, error);
}

GS::ObjectState Point (edit::Vec3 p)
{
    GS::ObjectState os;
    os.Add ("x", p.x);
    os.Add ("y", p.y);
    os.Add ("z", p.z);
    return os;
}

GS::ObjectState Export (const edit::Document& doc)
{
    GS::Array<GS::ObjectState> polylines, paths;
    for (const auto& e : doc.entities) {
        GS::ObjectState os;
        os.Add ("id", static_cast<GS::Int64> (e.id));
        os.Add ("kind", GS::UniString (e.kind == edit::Kind::Extrusion ? "extrusion" : "polyline"));
        os.Add ("role", GS::UniString (e.role.c_str (), CC_UTF8));
        os.Add ("closed", e.closed);
        os.Add ("height", e.height);
        GS::Array<GS::ObjectState> points;
        for (const auto& v : e.vertices)
            points.Push (Point (v.p));
        os.Add ("points", points);
        polylines.Push (os);
    }
    for (const auto& p : doc.paths) {
        GS::ObjectState os;
        edit::AlignmentGeometry g;
        std::string error;
        os.Add ("id", static_cast<GS::Int64> (p.id));
        os.Add ("role", GS::UniString (p.role.c_str (), CC_UTF8));
        os.Add ("length", edit::BuildAlignment (p.points, g, error) ? g.length : 0.0);
        GS::Array<GS::ObjectState> points, profile, sections;
        for (const auto& c : p.points) {
            GS::ObjectState q;
            q.Add ("x", c.p.x);
            q.Add ("y", c.p.y);
            q.Add ("radius", c.radius);
            points.Push (q);
        }
        for (const auto& b : p.profile) {
            GS::ObjectState q;
            q.Add ("station", b.station);
            q.Add ("z", b.z);
            q.Add ("curve", b.curve);
            q.Add ("fixed", b.fixed);
            q.Add ("binding", GS::UniString (b.binding.c_str (), CC_UTF8));
            profile.Push (q);
        }
        for (const auto& r : p.sections) {
            GS::ObjectState q;
            q.Add ("station", r.station);
            q.Add ("widthLeft", r.widthLeft);
            q.Add ("widthRight", r.widthRight);
            q.Add ("crossfall", r.crossfall);
            q.Add ("fall", GS::UniString (r.fall == edit::Fall::Left    ? "left"
                                          : r.fall == edit::Fall::Right ? "right"
                                                                        : "crown"));
            sections.Push (q);
        }
        os.Add ("points", points);
        os.Add ("profile", profile);
        os.Add ("sections", sections);
        paths.Push (os);
    }
    GS::ObjectState out;
    out.Add ("polylines", polylines);
    out.Add ("paths", paths);
    return out;
}

void Republish ()
{
    archviz::overlaycontrol::PublishLayers ();
    archviz::overlayinput::RequestLayout (archviz::overlayinput::View::ThreeD);
    archviz::overlayinput::RequestLayout (archviz::overlayinput::View::Plan);
}

void Stop (hud::State& state, const char* reason)
{
    hud::StopVirtualEditor (state, reason);
    archviz::overlaylayers::Clear (el::kLayer);
}

class VirtualEditorCommand : public MainThreadCommand {
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        auto state = archviz::guesttext::HudState ();
        GS::UniString action;
        params.Get ("action", action);
        GS::ObjectState result;
        if (const eh::Session* s = hud::VirtualEditor (static_cast<const hud::State&> (*state));
            s != nullptr && (action == "state" || action == "stop"))
            result.Add ("document", Export (s->edits.Doc ()));
        if (action == "stop")
            Stop (*state, "Tapioca.VirtualEditor stop");
        else if (action == "start") {
            const std::string role = Role (params, "");
            edit::Document doc;
            edit::SnapContext context;
            std::string error;
            double planeZ = 0.0;
            bool planeLocked = false;
            if (params.Contains ("planeZ") && !ReadFiniteNumber (params, "planeZ", planeZ))
                return NativeCommandResult::Failure (
                    EVP_FAIL ("Working-plane elevation must be finite.", "opening the virtual editor"));
            params.Get ("planeLocked", planeLocked);
            if (!Seed (params, role, doc, context, error))
                return NativeCommandResult::Failure (
                    EVP_FAIL (GS::UniString (error.c_str (), CC_UTF8), "opening the virtual editor"));
            if (!hud::StartVirtualEditor (*state, std::move (doc), role, std::move (context), planeZ, planeLocked))
                return NativeCommandResult::Failure (
                    EVP_FAIL ("The working plane was refused.", "opening the virtual editor"));
            auto& session = *hud::VirtualEditor (*state);
            session.owner = archviz::overlaycontrol::RunningCommand ();
            GS::UniString tool;
            if (params.Get ("tool", tool))
                session.tool = tool == "polyline" ? eh::Tool::Polyline
                               : tool == "path"   ? eh::Tool::Path
                               : tool == "curve"  ? eh::Tool::Curve
                                                  : eh::Tool::Select;
            archviz::overlaylayers::Layer layer;
            eh::Follow (session, layer);
            error = archviz::overlaylayers::Validate (layer);
            if (!error.empty ()) {
                Stop (*state, "its layer was refused");
                return NativeCommandResult::Failure (
                    EVP_FAIL (GS::UniString (error.c_str (), CC_UTF8), "publishing the virtual editor"));
            }
            archviz::overlaylayers::Set (std::move (layer));
        }
        const bool keys = hud::VirtualEditorActive (*state) && hud::EditLocked (*state) && hud::ContentShown (*state) &&
                          hud::LayerShown (*state, el::kLayer);
        if (keys && !archviz::overlayinput::EnableRoofEditKeys (true)) {
            Stop (*state, "keyboard ownership was refused");
            Republish ();
            return NativeCommandResult::Failure (
                EVP_FAIL ("Could not acquire scoped keyboard ownership; the virtual editor was refused.",
                          "opening the virtual editor"));
        }
        Republish ();
        const eh::Session* s = hud::VirtualEditor (static_cast<const hud::State&> (*state));
        result.Add ("active", s != nullptr);
        result.Add ("locked", hud::EditLocked (*state));
        if (s != nullptr) {
            result.Add ("revision", static_cast<GS::Int64> (s->edits.Doc ().revision));
            result.Add ("undoDepth", static_cast<GS::Int64> (s->edits.UndoDepth ()));
            result.Add ("message", GS::UniString (s->message.c_str (), CC_UTF8));
        }
        return result;
    }
};

const char* DragName (eh::Drag drag)
{
    switch (drag) {
        case eh::Drag::None:
            return "none";
        case eh::Drag::Part:
            return "part";
        case eh::Drag::Handle:
            return "handle";
        case eh::Drag::Marquee:
            return "marquee";
        case eh::Drag::ProfileBreak:
            return "profileBreak";
        case eh::Drag::ProfileTangent:
            return "profileTangent";
        case eh::Drag::Corner:
            return "corner";
        case eh::Drag::Kerb:
            return "kerb";
        case eh::Drag::Bezier:
            return "bezier";
        case eh::Drag::Bend:
            return "bend";
    }
    return "none";
}

const char* KindName (edit::SelectionKind kind)
{
    switch (kind) {
        case edit::SelectionKind::None:
            return "none";
        case edit::SelectionKind::Vertex:
            return "vertex";
        case edit::SelectionKind::Segment:
            return "segment";
        case edit::SelectionKind::Face:
            return "face";
        case edit::SelectionKind::Entity:
            return "entity";
        case edit::SelectionKind::Mixed:
            return "mixed";
        case edit::SelectionKind::PathPoint:
            return "pathPoint";
        case edit::SelectionKind::Break:
            return "break";
        case edit::SelectionKind::Range:
            return "range";
    }
    return "none";
}

// Read-only: what the editor holds and what it is doing, for a live run's log.
class EditorStateCommand : public MainThreadCommand {
    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        auto state = archviz::guesttext::HudState ();
        const eh::Session* s = hud::VirtualEditor (static_cast<const hud::State&> (*state));
        GS::ObjectState result;
        result.Add ("active", s != nullptr);
        result.Add ("locked", hud::EditLocked (*state));
        result.Add ("layerShown", hud::ContentShown (*state) && hud::LayerShown (*state, el::kLayer));
        if (s == nullptr)
            return result;
        const char* tools[] = { "select", "polyline", "path", "curve" };
        const char* modes[] = { "move", "rotate", "scale", "stretch", "extrude", "gumball" };
        const char* tracker[] = { "hidden", "showing", "editing" };
        result.Add ("tool", GS::UniString (tools[int (s->tool)]));
        result.Add ("gizmo", GS::UniString (modes[int (s->gizmo)]));
        result.Add ("drag", GS::UniString (DragName (s->drag)));
        result.Add ("gesture", s->edits.InGesture ());
        result.Add ("selection", static_cast<GS::Int64> (s->selection.Items ().size ()));
        result.Add ("selectionKind", GS::UniString (KindName (s->selection.Kind ())));
        result.Add ("hovering", s->selection.hover.entity != edit::kNoId);
        result.Add ("revision", static_cast<GS::Int64> (s->edits.Doc ().revision));
        result.Add ("publishedRevision", static_cast<GS::Int64> (s->publishedRevision));
        result.Add ("undoDepth", static_cast<GS::Int64> (s->edits.UndoDepth ()));
        result.Add ("tracker", GS::UniString (tracker[int (s->tracker.State ())]));
        result.Add ("drawing", static_cast<GS::Int64> (s->drawing.size ()));
        result.Add ("entities", static_cast<GS::Int64> (s->edits.Doc ().entities.size ()));
        result.Add ("paths", static_cast<GS::Int64> (s->edits.Doc ().paths.size ()));
        result.Add ("snapPoints", static_cast<GS::Int64> (s->context.points.size ()));
        result.Add ("snapSegments", static_cast<GS::Int64> (s->context.segments.size ()));
        result.Add ("planeZ", s->plane.origin.z);
        result.Add ("planeLocked", s->plane.locked);
        result.Add ("planeSticky", s->plane.sticky);
        result.Add ("planeSource", GS::UniString (s->plane.source == edit::PlaneSource::Face ? "face" : "worldXY"));
        result.Add ("planeOrigin", Point (s->plane.origin));
        result.Add ("planeU", Point (s->plane.u));
        result.Add ("planeV", Point (s->plane.v));
        result.Add ("planeNormal", Point (s->plane.normal));
        if (s->plane.source == edit::PlaneSource::Face) {
            result.Add ("planeSourceEntity", static_cast<GS::Int64> (s->plane.sourceRef.entity));
            result.Add ("planeSourceFace", static_cast<GS::Int64> (s->plane.sourceRef.item));
            result.Add ("planeSourceRevision", static_cast<GS::Int64> (s->plane.sourceRevision));
        }
        result.Add ("planeCaptured", s->capturedPlane.has_value ());
        result.Add ("message", GS::UniString (s->message.c_str (), CC_UTF8));
        return result;
    }
};

} // namespace

NativeCommandRegistrations GetVirtualEditorCommandRegistrations ()
{
    static const NativeCommandRegistration registrations[] = {
        // A UI lifecycle that must release its borrowed lock and keys even after a Run is
        // cancelled, as the roof editor's does; it never writes to the model.
        { "VirtualEditor", &MakeRegisteredNativeCommand<VirtualEditorCommand>, true,
          R"json({"type":"object","properties":{
                "action":{"type":"string","enum":["start","stop","state"]},"role":{"type":"string"},
                "tool":{"type":"string","enum":["select","polyline","path","curve"]},
                "planeZ":{"type":"number"},"planeLocked":{"type":"boolean"},
                "polylines":{"type":"array","maxItems":1000,"items":{"type":"object","properties":{"points":{"type":"array","minItems":2,"maxItems":20000,"items":{"$ref":"#Point3D"}},"closed":{"type":"boolean"},"role":{"type":"string"}},"required":["points"],"additionalProperties":false}},
                "boxes":{"type":"array","maxItems":1000,"items":{"type":"object","properties":{"outline":{"type":"array","minItems":3,"maxItems":20000,"items":{"$ref":"#Point3D"}},"height":{"type":"number","exclusiveMinimum":0},"role":{"type":"string"}},"required":["outline","height"],"additionalProperties":false}},
                "paths":{"type":"array","maxItems":200,"items":{"type":"object","properties":{"points":{"type":"array","minItems":2,"maxItems":2000,"items":{"$ref":"#Point3D"}},"width":{"type":"number","exclusiveMinimum":0,"maximum":100},"role":{"type":"string"}},"required":["points"],"additionalProperties":false}},
                "snapPoints":{"type":"array","maxItems":100000,"items":{"$ref":"#Point3D"}},
                "snapSegments":{"type":"array","maxItems":100000,"items":{"type":"array","minItems":2,"maxItems":2,"items":{"$ref":"#Point3D"}}}
              },"required":["action"],"additionalProperties":false})json",
          R"json({"type":"object","properties":{"active":{"type":"boolean"},"locked":{"type":"boolean"},"revision":{"type":"integer"},"undoDepth":{"type":"integer"},"message":{"type":"string"},"document":{"type":"object","properties":{"polylines":{"type":"array","items":{"type":"object","properties":{"id":{"type":"integer"},"kind":{"type":"string","enum":["polyline","extrusion"]},"role":{"type":"string"},"closed":{"type":"boolean"},"height":{"type":"number"},"points":{"type":"array","items":{"$ref":"#Point3D"}}},"required":["id","kind","role","closed","height","points"],"additionalProperties":false}},"paths":{"type":"array","items":{"type":"object","properties":{"id":{"type":"integer"},"role":{"type":"string"},"length":{"type":"number"},"points":{"type":"array","items":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"radius":{"type":"number"}},"required":["x","y","radius"],"additionalProperties":false}},"profile":{"type":"array","items":{"type":"object","properties":{"station":{"type":"number"},"z":{"type":"number"},"curve":{"type":"number"},"fixed":{"type":"boolean"},"binding":{"type":"string"}},"required":["station","z","curve","fixed","binding"],"additionalProperties":false}},"sections":{"type":"array","items":{"type":"object","properties":{"station":{"type":"number"},"widthLeft":{"type":"number"},"widthRight":{"type":"number"},"crossfall":{"type":"number"},"fall":{"type":"string","enum":["crown","left","right"]}},"required":["station","widthLeft","widthRight","crossfall","fall"],"additionalProperties":false}}},"required":["id","role","length","points","profile","sections"],"additionalProperties":false}}},"required":["polylines","paths"],"additionalProperties":false}},"required":["active","locked"],"additionalProperties":false})json" },
        { "EditorState", &MakeRegisteredNativeCommand<EditorStateCommand>, true,
          R"json({"type":"object","properties":{},"additionalProperties":false})json",
          R"json({"type":"object","properties":{
                "active":{"type":"boolean"},"locked":{"type":"boolean"},"layerShown":{"type":"boolean"},
                "tool":{"type":"string","enum":["select","polyline","path","curve"]},
                "gizmo":{"type":"string","enum":["move","rotate","scale","stretch","extrude","gumball"]},
                "drag":{"type":"string","enum":["none","part","handle","marquee","profileBreak","profileTangent","corner","kerb","bezier","bend"]},
                "gesture":{"type":"boolean"},"selection":{"type":"integer"},
                "selectionKind":{"type":"string","enum":["none","vertex","segment","face","entity","mixed","pathPoint","break","range"]},
                "hovering":{"type":"boolean"},"revision":{"type":"integer"},"publishedRevision":{"type":"integer"},
                "undoDepth":{"type":"integer"},"tracker":{"type":"string","enum":["hidden","showing","editing"]},
                "drawing":{"type":"integer"},"entities":{"type":"integer"},"paths":{"type":"integer"},
                "snapPoints":{"type":"integer"},"snapSegments":{"type":"integer"},
                "planeZ":{"type":"number"},"planeLocked":{"type":"boolean"},"planeSticky":{"type":"boolean"},
                "planeSource":{"type":"string","enum":["worldXY","face"]},"planeCaptured":{"type":"boolean"},
                "planeOrigin":{"$ref":"#Point3D"},"planeU":{"$ref":"#Point3D"},
                "planeV":{"$ref":"#Point3D"},"planeNormal":{"$ref":"#Point3D"},
                "planeSourceEntity":{"type":"integer"},"planeSourceFace":{"type":"integer"},
                "planeSourceRevision":{"type":"integer"},
                "message":{"type":"string"}
              },"required":["active","locked","layerShown"],"additionalProperties":false})json" },
    };
    return MakeRegistrationView (registrations);
}

} // namespace geomsrv
