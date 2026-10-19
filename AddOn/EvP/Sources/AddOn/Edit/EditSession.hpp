#ifndef EVP_EDIT_EDITSESSION_HPP
#define EVP_EDIT_EDITSESSION_HPP

// One editing session: the document, its gesture state and its own undo history,
// independent of Archicad's. Nothing here writes to the model; a host publishes
// `doc` whenever `doc.revision` moves and commits only on an explicit accept.
#include "Edit/EditDocument.hpp"
#include <functional>
#include <string>
#include <vector>

namespace geomsrv::edit {

// An operation edits a document in place or refuses with a reason. Refusing must
// leave the caller free to discard the copy it was given.
using Operation = std::function<bool (Document&, std::string&)>;

// Begin -> Move* -> End | Cancel. Every Move is evaluated from the snapshot taken at
// Begin, never from the previous Move, so a pointer that returns to its start
// restores the start exactly and screen deltas never accumulate. A refused Move keeps
// the last valid state and says why in `message`. End records one undo entry for the
// whole gesture, and only if it changed something.
class Session {
  public:
    explicit Session (Document start = {}, size_t undoCap = 128);

    const Document& Doc () const
    {
        return doc;
    }
    const Document& Before () const
    {
        return before;
    }
    bool InGesture () const
    {
        return gesture;
    }
    const Ref& Target () const
    {
        return target;
    }
    const std::string& Message () const
    {
        return message;
    }

    // Fails (and changes nothing) during a gesture or when `target` names something
    // that no longer exists: a stale handle must not start an edit.
    bool Begin (const Ref& target = {});
    bool Move (const Operation& operation);
    bool End (bool joinPathEnds = true);
    void Cancel ();
    // A whole gesture in one call: a typed value, a context action, a key.
    bool Apply (const Operation& operation, bool joinPathEnds = true);

    bool CanUndo () const
    {
        return !undo.empty () && !gesture;
    }
    bool CanRedo () const
    {
        return !redo.empty () && !gesture;
    }
    bool Undo ();
    bool Redo ();
    size_t UndoDepth () const
    {
        return undo.size ();
    }

  private:
    bool Accept (Document candidate, const std::string& refusal);
    void Restore (Document state);

    Document doc, before;
    std::vector<Document> undo, redo;
    size_t cap;
    bool gesture = false, changed = false;
    Ref target;
    std::string message;
};

} // namespace geomsrv::edit
#endif
