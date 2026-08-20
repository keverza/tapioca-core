#include "ScreenshotCapture.hpp"

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "Location.hpp"
#include "Name.hpp"

#include <fstream>
#include <vector>

namespace geomsrv {

namespace {

// Read a file (given as an IO::Location) fully into memory.
bool ReadAllBytes (const IO::Location& loc, std::string& out, std::string& err)
{
    GS::UniString pathU;
    if (loc.ToPath (&pathU) != NoError) {
        err = "could not resolve screenshot temp path";
        return false;
    }
    const std::string path (pathU.ToCStr (0, GS::MaxUSize, CC_System).Get ());
    std::ifstream f (path, std::ios::binary);
    if (!f) {
        err = "screenshot file not written (Save failed?)";
        return false;
    }
    out.assign ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char> ());
    return !out.empty ();
}

// Verify the front window is the 3D model window.
bool RequireThreeDWindow (std::string& err)
{
    API_WindowInfo wi = {};
    if (ACAPI_Window_GetCurrentWindow (&wi) != NoError || wi.typeID != APIWind_3DModelID) {
        err = "switch to the 3D window first";
        return false;
    }
    return true;
}

// Save the current window to a PNG in the temp folder and read it back.
bool SaveCurrentWindowPng (const char* fileName, std::string& png, std::string& err)
{
    API_SpecFolderID folderId = API_TemporaryFolderID;
    IO::Location folder;
    if (ACAPI_ProjectSettings_GetSpecFolder (&folderId, &folder) != NoError) {
        err = "could not locate temp folder";
        return false;
    }
    IO::Location file (folder, IO::Name (fileName));

    API_FileSavePars fsp = {};
    fsp.fileTypeID = APIFType_PNGFile;
    fsp.file = &file;

    API_SavePars_Picture pic = {};
    pic.colorDepth = APIColorDepth_TC32;  // true color, 32-bit (Windows)
    pic.dithered = false;
    pic.view2D = false;                   // 3D window content
    pic.crop = false;
    pic.keepSelectionHighlight = false;

    const GSErrCode e = ACAPI_ProjectOperation_Save (&fsp, &pic);
    if (e != NoError) {
        err = "ACAPI_ProjectOperation_Save failed (" + std::to_string ((int) e) + ")";
        return false;
    }
    return ReadAllBytes (file, png, err);
}

// Force the 3D window to regenerate + redraw so a subsequent Save captures the
// up-to-date frame (e.g. after a projection change, or the very first capture).
void ForceRegenerate ()
{
    bool regenerate = true;
    ACAPI_View_Rebuild (&regenerate);
    ACAPI_View_Redraw ();
}

// Fit every model element into the current (3D) window — keeps the view
// direction, adjusts zoom/pan. Needed after a top-down projection change, whose
// identity matrix otherwise leaves the model off-frame.
void ZoomToFitAll ()
{
    GS::Array<API_Guid> all;
    if (ACAPI_Element_GetElemList (API_ZombieElemID, &all) == NoError && !all.IsEmpty ())
        ACAPI_View_ZoomToElements (&all);
}

void SetIdentityTopView (API_3DProjectionInfo& proj)
{
    // Carry the SUN across from whatever the user is currently looking through.
    // Both API_PerspPars and API_AxonoPars carry sunAngSets (azimuth, altitude,
    // date/time). We used to zero the whole axono struct, which set sunAltitude = 0
    // -> sun on the horizon -> every upward-facing surface unlit -> the top view
    // came out dark. Shadows follow the 3D style, which we never touch, so once the
    // sun is right the shading and shadows match the current view.
    const API_SunAngleSettings sun = proj.isPersp ? proj.u.persp.sunAngSets
                                                  : proj.u.axono.sunAngSets;

    proj.isPersp = false;
    proj.u.axono = API_AxonoPars {};            // zero, then restore what matters
    proj.u.axono.sunAngSets = sun;              // <- keep the sun (and its shadows)
    proj.u.axono.projMod = API_Projection_XY;   // top (XY plane), looking down -Z
    proj.u.axono.azimuth = 0.0;
    double* m = proj.u.axono.tranmat.tmx;
    for (int i = 0; i < 12; ++i) m[i] = 0.0;
    m[0] = 1.0; m[5] = 1.0; m[10] = 1.0;        // identity rotation, no translation
}

} // namespace

bool CaptureCurrentView (std::string& pngBytes, std::string& err)
{
    if (!RequireThreeDWindow (err))
        return false;
    ForceRegenerate ();   // make sure the 3D frame is up to date before saving
    return SaveCurrentWindowPng ("evp_current.png", pngBytes, err);
}

bool CaptureTopDown (std::string& pngBytes, std::string& err)
{
    if (!RequireThreeDWindow (err))
        return false;

    API_3DProjectionInfo saved = {};
    if (ACAPI_View_Get3DProjectionSets (&saved) != NoError) {
        err = "could not read 3D projection settings";
        return false;
    }

    API_3DProjectionInfo top = saved;
    SetIdentityTopView (top);
    if (ACAPI_View_Change3DProjectionSets (&top) != NoError) {
        err = "could not set top-down projection";
        return false;
    }
    ForceRegenerate ();   // apply the new projection to the rendered frame
    ZoomToFitAll ();      // frame the whole model (top view is otherwise off-screen)
    ACAPI_View_Redraw ();

    const bool ok = SaveCurrentWindowPng ("evp_top.png", pngBytes, err);

    // Always restore the user's original projection (and redraw), even on failure.
    ACAPI_View_Change3DProjectionSets (&saved);
    ForceRegenerate ();
    return ok;
}

} // namespace geomsrv
