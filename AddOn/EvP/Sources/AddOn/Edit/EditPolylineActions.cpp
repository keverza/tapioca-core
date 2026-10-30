// The shape editor's context actions: rows keyed by what is selected, each building one
// operation from the edits in EditPolyline.cpp, EditShape.cpp and EditPathEdit.cpp.
#include "Edit/EditPolyline.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditShape.hpp"
#include <cmath>

namespace geomsrv::edit {

std::vector<Action> PolylineActions (const ActionDefaults& d)
{
    using Refs = std::vector<Ref>;
    const uint32_t vertex = Bit (SelectionKind::Vertex), segment = Bit (SelectionKind::Segment),
                   entity = Bit (SelectionKind::Entity);
    std::vector<Action> table;
    table.push_back (
        { "insert-vertex", "Insert vertex", segment, 0xFu, 1, 1, [] (const Document& doc, const Refs& refs) {
             Vec3 mid;
             const Ref r = refs.front ();
             if (!PointOf (doc, r, mid))
                 return Operation {};
             return Operation (
                 [r, mid] (Document& x, std::string& error) { return InsertVertex (x, r.entity, r.item, mid, error); });
         } });
    table.push_back ({ "delete-vertex", "Delete vertex", vertex, 0xFu, 1, 0, [] (const Document&, const Refs& refs) {
                          return Operation ([refs] (Document& x, std::string& error) {
                              for (const auto& r : refs)
                                  if (!DeleteVertex (x, r.entity, r.item, error))
                                      return false;
                              return true;
                          });
                      } });
    // A fillet is a live corner: it keeps its radius, editable by its grip or by a typed
    // value, until it is baked into points.
    table.push_back ({ "fillet", "Round corner", vertex, 0xFu, 1, 0, [d] (const Document&, const Refs& refs) {
                          return Operation ([refs, d] (Document& x, std::string& error) {
                              for (const auto& r : refs)
                                  if (!SetCornerRadius (x, r.entity, { r.item }, d.filletRadius, error))
                                      return false;
                              return true;
                          });
                      } });
    table.push_back ({ "sharpen", "Sharp corner", vertex, 0xFu, 1, 0, [] (const Document& doc, const Refs& refs) {
                          bool any = false;
                          for (const auto& r : refs)
                              if (const Entity* e = Find (doc, r.entity))
                                  for (const auto& v : e->vertices)
                                      any = any || (v.id == r.item && v.radius > 0.0);
                          if (!any)
                              return Operation {};
                          return Operation ([refs] (Document& x, std::string& error) {
                              for (const auto& r : refs)
                                  if (!SetCornerRadius (x, r.entity, { r.item }, 0.0, error))
                                      return false;
                              return true;
                          });
                      } });
    // A centreline drawn as a polyline -- lines and rounded corners -- becomes a path.
    table.push_back (
        { "make-path", "Make a path along it", entity, 0xFu, 1, 0, [d] (const Document& doc, const Refs& refs) {
             for (const auto& r : refs) {
                 const Entity* e = Find (doc, r.entity);
                 if (e == nullptr || e->kind != Kind::Polyline || r.part != Part::Entity)
                     return Operation {};
             }
             return Operation ([refs, d] (Document& x, std::string& error) {
                 for (const auto& r : refs)
                     if (PathFromPolyline (x, r.entity, d.pathWidth, error) == kNoId)
                         return false;
                 return true;
             });
         } });
    table.push_back ({ "bake-corners", "Turn rounded corners into points", entity, 0xFu, 1, 0,
                       [] (const Document& doc, const Refs& refs) {
                           bool any = false;
                           for (const auto& r : refs)
                               if (const Entity* e = Find (doc, r.entity))
                                   for (const auto& v : e->vertices)
                                       any = any || v.radius > 0.0;
                           if (!any)
                               return Operation {};
                           return Operation ([refs] (Document& x, std::string& error) {
                               for (const auto& r : refs) {
                                   std::vector<std::pair<Id, double>> rounded;
                                   if (const Entity* e = Find (x, r.entity))
                                       for (size_t i = 0; i < e->vertices.size (); ++i)
                                           if (e->vertices[i].radius > 0.0)
                                               rounded.push_back ({ e->vertices[i].id, DrawnRadius (*e, i) });
                                   for (const auto& [id, radius] : rounded) {
                                       Entity* e = Find (x, r.entity);
                                       const int i = VertexIndex (*e, id);
                                       if (i < 0)
                                           continue;
                                       e->vertices[size_t (i)].radius = 0.0;
                                       if (radius > 0.0 && !Fillet (x, r.entity, id, radius, 8, error))
                                           return false;
                                   }
                               }
                               return true;
                           });
                       } });
    const auto anchors = [] (const Document& doc, const Refs& refs, bool smooth) {
        bool any = false; // something to change: a corner to smooth, or handles to retract
        for (const auto& r : refs)
            if (const Entity* e = Find (doc, r.entity))
                for (const auto& v : e->vertices)
                    if (v.id == r.item)
                        any = any || (smooth ? !v.smooth
                                             : (v.smooth || std::hypot (v.in.x, v.in.y) > 0.0 ||
                                                std::hypot (v.out.x, v.out.y) > 0.0));
        if (!any)
            return Operation {};
        return Operation ([refs, smooth] (Document& x, std::string& error) {
            for (const auto& r : refs)
                if (!SetSmooth (x, r.entity, r.item, smooth, error))
                    return false;
            return true;
        });
    };
    table.push_back ({ "smooth", "Smooth point", vertex, 0xFu, 1, 0,
                       [anchors] (const Document& doc, const Refs& refs) { return anchors (doc, refs, true); } });
    table.push_back ({ "corner", "Corner point", vertex, 0xFu, 1, 0,
                       [anchors] (const Document& doc, const Refs& refs) { return anchors (doc, refs, false); } });
    const auto segments = [] (const Document& doc, const Refs& refs, bool curved) {
        bool any = false;
        for (const auto& r : refs)
            if (const Entity* e = Find (doc, r.entity)) {
                const int i = SegmentIndex (*e, r.item);
                any = any || (i >= 0 && SegmentCurved (*e, size_t (i)) != curved);
            }
        if (!any)
            return Operation {};
        return Operation ([refs, curved] (Document& x, std::string& error) {
            for (const auto& r : refs)
                if (!SetSegmentCurved (x, r.entity, r.item, curved, error))
                    return false;
            return true;
        });
    };
    table.push_back ({ "curve", "Make curve", segment, 0xFu, 1, 0,
                       [segments] (const Document& doc, const Refs& refs) { return segments (doc, refs, true); } });
    table.push_back ({ "straighten", "Make straight", segment, 0xFu, 1, 0,
                       [segments] (const Document& doc, const Refs& refs) { return segments (doc, refs, false); } });
    table.push_back ({ "chamfer", "Chamfer", vertex, 0xFu, 1, 0, [d] (const Document&, const Refs& refs) {
                          return Operation ([refs, d] (Document& x, std::string& error) {
                              for (const auto& r : refs)
                                  if (!Chamfer (x, r.entity, r.item, d.chamferDistance, error))
                                      return false;
                              return true;
                          });
                      } });
    table.push_back ({ "split", "Split here", vertex, 0xFu, 1, 1, [] (const Document&, const Refs& refs) {
                          const Ref r = refs.front ();
                          return Operation ([r] (Document& x, std::string& error) {
                              return SplitAtVertex (x, r.entity, r.item, error);
                          });
                      } });
    table.push_back ({ "join", "Join", entity, 0xFu, 2, 2, [d] (const Document&, const Refs& refs) {
                          const Id a = refs[0].entity, b = refs[1].entity;
                          return Operation ([a, b, d] (Document& x, std::string& error) {
                              return Join (x, a, b, d.joinTolerance, error);
                          });
                      } });
    // Whole polylines, each one the edit applies to; offered while any selected one has
    // something to change.
    using Change = bool (*) (Document&, Id, std::string&);
    const auto each = [] (const Document& doc, const Refs& refs, bool (*wants) (const Entity&), Change change) {
        std::vector<Id> ids;
        for (const auto& r : refs)
            if (const Entity* e = Find (doc, r.entity); e != nullptr && r.part == Part::Entity && wants (*e))
                ids.push_back (e->id);
        if (ids.empty ())
            return Operation {};
        return Operation ([ids, change] (Document& x, std::string& error) {
            for (const Id id : ids)
                if (!change (x, id, error))
                    return false;
            return true;
        });
    };
    table.push_back (
        { "close", "Close outline", entity, 0xFu, 1, 0, [each] (const Document& doc, const Refs& refs) {
             return each (
                 doc, refs,
                 [] (const Entity& e) { return e.kind == Kind::Polyline && !e.closed && e.vertices.size () >= 3; },
                 &Close);
         } });
    table.push_back ({ "open", "Open outline", entity, 0xFu, 1, 0, [each] (const Document& doc, const Refs& refs) {
                          return each (
                              doc, refs, [] (const Entity& e) { return e.kind == Kind::Polyline && e.closed; }, &Open);
                      } });
    table.push_back (
        { "reverse", "Reverse direction", entity, 0xFu, 1, 0, [each] (const Document& doc, const Refs& refs) {
             return each (doc, refs, [] (const Entity& e) { return e.kind == Kind::Polyline; }, &ReverseDirection);
         } });
    // A parallel copy of each selected outline; the copy is what the edit adds.
    table.push_back ({ "offset", "Offset copy", entity, 0xFu, 1, 0, [d] (const Document& doc, const Refs& refs) {
                          std::vector<Id> ids;
                          for (const auto& r : refs)
                              if (Find (doc, r.entity) != nullptr && r.part == Part::Entity)
                                  ids.push_back (r.entity);
                          if (ids.empty () || d.offsetDistance == 0.0)
                              return Operation {};
                          return Operation ([ids, d] (Document& x, std::string& error) {
                              for (const Id id : ids) {
                                  const Entity& e = *Find (x, id);
                                  // OffsetEntity runs to the left, and an extrusion's profile
                                  // outward; a ring of either winding goes outside.
                                  const bool outside = e.closed && e.kind == Kind::Polyline;
                                  const double left =
                                      outside && SignedAreaXY (e.vertices) > 0.0 ? -d.offsetDistance : d.offsetDistance;
                                  if (!OffsetEntity (x, id, left, error))
                                      return false;
                              }
                              return true;
                          });
                      } });
    // The first selected polyline is cut back to, or lengthened to meet, the second. Of its
    // two ends, a trim takes the one that leaves the longer polyline and an extension the
    // one that reaches the other sooner.
    const auto planLength = [] (const Entity& e) {
        double length = 0.0;
        for (size_t i = 0; i < SegmentCount (e); ++i) {
            size_t from = 0, to = 0;
            SegmentEnds (e, i, from, to);
            length += DistanceXY (e.vertices[from].p, e.vertices[to].p);
        }
        return length;
    };
    using EndEdit = bool (*) (Document&, Id, bool, Id, std::string&);
    const auto toOther = [planLength] (const Document& doc, const Refs& refs, EndEdit edit, bool keepLonger) {
        const Entity* e = Find (doc, refs[0].entity);
        if (e == nullptr || Find (doc, refs[1].entity) == nullptr || e->closed || e->kind != Kind::Polyline ||
            refs[0].part != Part::Entity || refs[1].part != Part::Entity)
            return Operation {};
        const Id id = refs[0].entity, other = refs[1].entity;
        return Operation ([id, other, edit, keepLonger, planLength] (Document& x, std::string& error) {
            Document start = x, end = x;
            std::string whyStart, whyEnd;
            const bool atStart = edit (start, id, true, other, whyStart);
            const bool atEnd = edit (end, id, false, other, whyEnd);
            if (!atStart && !atEnd) {
                error = whyStart;
                return false;
            }
            bool useStart = atStart;
            if (atStart && atEnd) {
                const double a = planLength (*Find (start, id)), b = planLength (*Find (end, id));
                useStart = keepLonger ? a >= b : a <= b;
            }
            x = useStart ? std::move (start) : std::move (end);
            return true;
        });
    };
    table.push_back (
        { "trim", "Trim to the other", entity, 0xFu, 2, 2,
          [toOther] (const Document& doc, const Refs& refs) { return toOther (doc, refs, &TrimAtEntity, true); } });
    table.push_back (
        { "extend", "Extend to the other", entity, 0xFu, 2, 2,
          [toOther] (const Document& doc, const Refs& refs) { return toOther (doc, refs, &ExtendToEntity, false); } });
    table.push_back (
        { "merge", "Merge duplicate vertices", entity, 0xFu, 1, 0, [d] (const Document&, const Refs& refs) {
             return Operation ([refs, d] (Document& x, std::string& error) {
                 for (const auto& r : refs)
                     if (!MergeVertices (x, r.entity, d.mergeTolerance, error))
                         return false;
                 return true;
             });
         } });
    return table;
}

} // namespace geomsrv::edit
