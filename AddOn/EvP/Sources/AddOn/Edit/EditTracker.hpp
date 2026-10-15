#ifndef EVP_EDIT_EDITTRACKER_HPP
#define EVP_EDIT_EDITTRACKER_HPP

// Typed numbers for the editor: a cursor-following set of fields the user can type
// into at any moment of a gesture, and dimension bindings that turn a typed value
// into the same operation a drag would make. One apply path for both, so a typed
// length and a dragged length can never disagree.
#include "Edit/EditSession.hpp"
#include <optional>
#include <string>
#include <vector>

namespace geomsrv::edit {

enum class Quantity : uint8_t { X, Y, Z, Length, Angle, Elevation, Slope, Width, Distance, Height, Radius, Factor };
const char* QuantityName (Quantity quantity);
const char* QuantityUnit (Quantity quantity); // "m", "deg", "%" or "x" (a scale factor)

// Plain numbers only for now: an optional sign, digits, one decimal point or comma.
// Units, ratios and expressions belong behind this one function when they come.
bool ParseNumber (const std::string& text, double& value);

struct TrackerItem {
    Quantity quantity = Quantity::Length;
    double value = 0.0; // live value, refreshed by the host every move
    bool editable = true;
    bool locked = false; // fixed by Tab: the drag keeps it while the rest follows
    double lockedValue = 0.0;
};

struct TrackerKey {
    enum class Kind : uint8_t { Char, Tab, BackTab, Enter, Escape, Backspace };
    Kind kind = Kind::Char;
    char c = 0;
};

enum class TrackerState : uint8_t { Hidden, Showing, Editing };

// Showing --digit/sign/point--> Editing(focused item)
// Editing --Enter (valid)--> Commit; --Tab (valid)--> lock + next item;
//         --Esc--> Showing; --Backspace on empty--> Showing
// Showing --Tab--> next item; --Esc--> CancelGesture
class Tracker {
  public:
    enum class Outcome : uint8_t { None, Locked, Commit, Invalid, CancelGesture };
    struct Event {
        Outcome outcome = Outcome::None;
        size_t item = 0;
        double value = 0.0;
    };

    // Shows a new set of fields. Showing the same quantities again only refreshes
    // their live values, so focus, typed text and locks survive every pointer move.
    void Show (const std::vector<TrackerItem>& items, size_t defaultItem = 0);
    void Hide ();
    Event Key (const TrackerKey& key);

    TrackerState State () const;
    size_t Focus () const;
    const std::string& Text () const;
    const std::vector<TrackerItem>& Items () const;
    std::optional<double> Locked (Quantity quantity) const;

  private:
    void Advance (int step);

    std::vector<TrackerItem> items;
    TrackerState state = TrackerState::Hidden;
    size_t focus = 0;
    std::string text;
};

// A dimension shown on the geometry and editable by clicking its label.
struct Dimension {
    Ref ref;
    Quantity quantity = Quantity::Length;
    // A segment's end stays put and its start moves (default: the start stays). A locked
    // end always stays. The angle is the direction from the end that stays to the one that
    // moves, so the arc drawn at the fixed end reads what is typed.
    bool fromEnd = false;
};
// The segment a dimension measures, as its edit sees it: the end that stays and the one
// that moves (`fixedIsStart` false when the end stays). False when the segment is gone.
bool SegmentEndsFor (const Document& doc, const Dimension& dimension, Vec3& fixed, Vec3& moving, bool& fixedIsStart);
// The dimensions worth showing for a selected item, in display order.
std::vector<Dimension> DimensionsFor (const Document& doc, const Ref& ref);
bool ReadDimension (const Document& doc, const Dimension& dimension, double& value);
// The operation that makes the dimension read `value`. Lengths and slopes keep the
// segment's start (or its locked end) fixed; angles turn about that same end.
Operation WriteDimension (const Dimension& dimension, double value);

} // namespace geomsrv::edit
#endif
