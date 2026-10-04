#include "ParamDateTimeProbe.hpp"
#include "ParamPanel.hpp"
#include "Python/PathUtils.hpp"
#include "GSTime.hpp"

namespace evp {

void LogDateTimeProbe (const GS::UniString& name, const char* stage, DG::DateTime& control)
{
    const GSTime raw = control.GetValue ();
    GSTimeRecord decoded = {};
    const GSErr decodeError = TIGetTimeRecord (raw, &decoded);
    if (decodeError != NoError) {
        AppendTextLine (
            ScanLogPath (),
            GS::UniString::Printf ("  DateTime probe '%T' %s: raw=%d min=%d max=%d decode=%d (no decoded record)",
                                   name.ToPrintf (), stage, (int) raw, (int) control.GetMin (), (int) control.GetMax (),
                                   (int) decodeError));
        return;
    }
    AppendTextLine (
        ScanLogPath (),
        GS::UniString::Printf ("  DateTime probe '%T' %s: raw=%d min=%d max=%d decode=%d record=%04u-%02u-%02u "
                               "%02u:%02u:%02u localISO=%T",
                               name.ToPrintf (), stage, (int) raw, (int) control.GetMin (), (int) control.GetMax (),
                               (int) decodeError, (unsigned) decoded.year, (unsigned) decoded.month,
                               (unsigned) decoded.day, (unsigned) decoded.hour, (unsigned) decoded.minute,
                               (unsigned) decoded.second, TIGetISO8601TimeString (raw).ToPrintf ()));
}

bool ParamPanel::HandleDateTimeChanged (const DG::DateTimeChangeEvent& ev, bool& reflow)
{
    for (ParamControl& pc : paramControls) {
        if (pc.kind == ParamControl::Kind::Calendar && ev.GetSource () == pc.control.get ()) {
            reflow = ApplyVisibility ();
            return true;
        }
        if (pc.kind != ParamControl::Kind::DateTimeProbe || ev.GetSource () != pc.control.get ())
            continue;
        LogDateTimeProbe (pc.name, "user changed", *static_cast<DG::DateTime*> (pc.control.get ()));
        AppendTextLine (ScanLogPath (),
                        GS::UniString::Printf ("  DateTime probe '%T' previousTime=%d previousISO=%T",
                                               pc.name.ToPrintf (), (int) ev.GetPreviousTime (),
                                               TIGetISO8601TimeString (ev.GetPreviousTime ()).ToPrintf ()));
        reflow = ApplyVisibility ();
        return true;
    }
    return false;
}

} // namespace evp
