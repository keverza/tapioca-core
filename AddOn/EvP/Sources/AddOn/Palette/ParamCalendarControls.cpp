#include "Palette/ParamCalendarControls.hpp"
#include "Palette/ParamCalendarValue.hpp"
#include "Palette/ParamPanel.hpp"
#include "GSTime.hpp"

namespace evp {
void BuildCalendarControl (ParamControl& pc, const GS::ObjectState& metadata, const DG::Panel& panel,
                           const DG::Rect& seed, DG::DateTimeObserver& observer)
{
    pc.kind = ParamControl::Kind::Calendar;
    auto control = std::make_unique<DG::CalendarControl> (panel, seed);
    GS::UniString defaultText;
    int year = 0, month = 0, day = 0;
    GSTime value = TIGetTime ();
    if (metadata.Get ("default", defaultText) &&
        ParseCalendarValue (std::string (defaultText.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ()), year, month, day)) {
        // Local noon avoids DST midnight discontinuities. Only the civil fields
        // are read back; raw GSTime never crosses the public command boundary.
        GSTimeRecord record (static_cast<unsigned short> (year), static_cast<unsigned short> (month), 0,
                             static_cast<unsigned short> (day), 12, 0, 0, 0);
        TIGetGSTime (&record, &value);
    }
    GSTime minimum = 0, maximum = 0;
    GSTimeRecord first (1902, 1, 0, 1, 0, 0, 0, 0), last (2037, 12, 0, 31, 23, 59, 59, 0);
    if (TIGetGSTime (&first, &minimum) == NoError)
        control->SetMin (minimum);
    if (TIGetGSTime (&last, &maximum) == NoError)
        control->SetMax (maximum);
    control->SetValue (value);
    control->Attach (observer);
    pc.control = std::move (control);
}

GS::UniString CalendarValueText (DG::DateTime& control)
{
    GSTimeRecord record;
    if (TIGetTimeRecord (control.GetValue (), &record) != NoError)
        return {};
    return GS::UniString::Printf ("%04u-%02u-%02u", unsigned (record.year), unsigned (record.month),
                                  unsigned (record.day));
}
} // namespace evp
