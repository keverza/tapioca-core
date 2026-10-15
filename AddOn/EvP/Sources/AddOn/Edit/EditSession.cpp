#include "Edit/EditSession.hpp"
#include <algorithm>

namespace geomsrv::edit {

// What a gesture changed, ignoring the bookkeeping (revision, id counter): every field
// of every entity and path, through their defaulted comparisons, so a field added to the
// model is compared without this function knowing it.
static bool SameContent (const Document& a, const Document& b)
{
    return a.paths == b.paths && a.entities == b.entities;
}

Session::Session (Document start, size_t undoCap)
    : doc (std::move (start)), before (doc), cap (std::max<size_t> (undoCap, 1))
{
}

bool Session::Begin (const Ref& ref)
{
    if (gesture) {
        message = "a gesture is already in progress";
        return false;
    }
    if (ref.entity != kNoId && !Exists (doc, ref)) {
        message = "the edited item no longer exists";
        return false;
    }
    before = doc;
    target = ref;
    gesture = true;
    changed = false;
    message.clear ();
    return true;
}

bool Session::Accept (Document candidate, const std::string& refusal)
{
    std::string error = refusal;
    if (error.empty ())
        Validate (candidate, error);
    if (!error.empty ()) {
        message = error;
        return false;
    }
    // Ids are never handed out twice, even by a candidate built from an older copy.
    candidate.nextId = std::max (candidate.nextId, doc.nextId);
    candidate.revision = doc.revision + 1;
    doc = std::move (candidate);
    changed = true;
    message.clear ();
    return true;
}

bool Session::Move (const Operation& operation)
{
    if (!gesture) {
        message = "no gesture in progress";
        return false;
    }
    Document candidate = before;
    candidate.nextId = std::max (candidate.nextId, doc.nextId);
    std::string error;
    if (!operation (candidate, error) && error.empty ())
        error = "the edit was refused";
    return Accept (std::move (candidate), error);
}

bool Session::End ()
{
    if (!gesture)
        return false;
    gesture = false;
    target = {};
    if (!changed || SameContent (doc, before))
        return false;
    undo.push_back (before);
    if (undo.size () > cap)
        undo.erase (undo.begin ());
    redo.clear ();
    return true;
}

void Session::Cancel ()
{
    if (!gesture)
        return;
    gesture = false;
    target = {};
    if (changed)
        Restore (before);
    changed = false;
}

bool Session::Apply (const Operation& operation)
{
    if (!Begin ())
        return false;
    if (!Move (operation)) {
        const std::string reason = message;
        Cancel ();
        message = reason;
        return false;
    }
    return End ();
}

void Session::Restore (Document state)
{
    state.nextId = std::max (state.nextId, doc.nextId);
    state.revision = doc.revision + 1;
    doc = std::move (state);
}

bool Session::Undo ()
{
    if (!CanUndo ())
        return false;
    redo.push_back (doc);
    Document state = std::move (undo.back ());
    undo.pop_back ();
    Restore (std::move (state));
    return true;
}

bool Session::Redo ()
{
    if (!CanRedo ())
        return false;
    undo.push_back (doc);
    Document state = std::move (redo.back ());
    redo.pop_back ();
    Restore (std::move (state));
    return true;
}

} // namespace geomsrv::edit
