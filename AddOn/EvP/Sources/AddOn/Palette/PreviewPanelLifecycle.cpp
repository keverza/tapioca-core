// OVERLAY-INVARIANTS.md §8: a hidden palette must release its copied trace too.
#include "Palette/PreviewPanel.hpp"

namespace evp {

void PreviewPanel::ForgetContext ()
{
    ReleaseMouseInput ();
    ClearTrace ();
    ClearOverlay ();
    if (kind == "3d")
        Request3DHost (previewpanel::Host::None);
    retainedGeneration = presentedGeneration = 0;
}

} // namespace evp
