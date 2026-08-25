#ifndef GEOMETRYSERVER_PALETTE_PREVIEWPANEL_HPP
#define GEOMETRYSERVER_PALETTE_PREVIEWPANEL_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

#include "Annotation/DrawList.hpp"
#include "Palette/PreviewPanelState.hpp"
#include "Palette/PreviewPlanCamera.hpp"
#include "PlanOverlay/PlanCanvasHost.hpp"

#include <cstdint>
#include <memory>

namespace evp {

// Fixed watch-trace footer. The shell remains the only DG observer; this object
// owns the items and state, and exposes routing methods like ResultsTable.
class PreviewPanel {
  public:
    PreviewPanel (const DG::Panel& panel, DG::CheckItemObserver& checkObserver, DG::ButtonItemObserver& buttonObserver,
                  DG::PopUpObserver& popupObserver, DG::ScrollBarObserver& scrollObserver,
                  DG::UserItemObserver& userItemObserver);
    ~PreviewPanel ();

    void Create ();
    void SetKind (const GS::UniString& kind);
    short Height () const;
    void PlaceAt (short left, short right, short bottom);
    bool PollRetained ();
    void SetEnabled (bool enabled);
    bool IsEnabled () const;
    void SetPaletteVisible (bool visible);

    bool HandleCheckItemChanged (const DG::CheckItemChangeEvent& event);
    bool HandleButtonClicked (const DG::ButtonClickEvent& event);
    bool HandlePopUpChanged (const DG::PopUpChangeEvent& event);
    bool HandleScrollBarChanged (const DG::ScrollBarChangeEvent& event);
    bool HandleScrollBarTracked (const DG::ScrollBarTrackEvent& event);
    bool HandleUserItemUpdate (const DG::UserItemUpdateEvent& event);
    bool HandleUserItemMouseDown (const DG::UserItemMouseDownEvent& event);
    bool HandleUserItemDoubleClicked (const DG::UserItemDoubleClickEvent& event);
    bool HandleUserItemMouseUp (const DG::UserItemMouseUpEvent& event);
    bool HandleUserItemMouseEntered (const DG::UserItemMouseEnteredEvent& event);
    bool HandleUserItemMouseMoved (const DG::UserItemMouseMoveEvent& event);
    bool HandleUserItemMouseExited (const DG::UserItemMouseExitedEvent& event);
    bool HandleWheelTracked (const DG::PanelWheelTrackEvent& event);

    bool IsCanvas (const DG::Item* item) const;

  private:
    void RebuildNodes ();
    bool SelectFirstAvailableFrame ();
    void ClearTrace ();
    void SelectFrame (size_t frameIndex);
    void PublishOverlay ();
    void ClearOverlay ();
    void UpdateLabels ();
    void Request3DHost (previewpanel::Host targetHost);
    void Poll3DHost ();
    bool StartBandHost ();
    void FinishExternalHost ();
    void SetPresentationStatus (const char* status);
    bool EmbeddedInputAvailable () const;
    void* CanvasWindow () const;
    bool RefreshPointerInput ();
    void ReleaseMouseInput ();
    bool PlanInputAvailable () const;
    void FitSelectedPlanFrame ();
    void UpdatePlanViewport ();
    void UpdateOpacity ();

    const DG::Panel& panel;
    DG::CheckItemObserver& checkObserver;
    DG::ButtonItemObserver& buttonObserver;
    DG::PopUpObserver& popupObserver;
    DG::ScrollBarObserver& scrollObserver;
    DG::UserItemObserver& userItemObserver;

    std::unique_ptr<DG::UserItem> canvas;
    std::unique_ptr<DG::CheckBox> enablePreviews;
    std::unique_ptr<DG::PopUp> nodeSelector;
    std::unique_ptr<DG::ScrollBar> frameScrubber;
    std::unique_ptr<DG::LeftText> frameLabel;
    std::unique_ptr<DG::LeftText> opacityLabel;
    std::unique_ptr<DG::ScrollBar> opacitySlider;
    std::unique_ptr<DG::Button> overlayButton;
    std::unique_ptr<DG::Button> popOutButton;
    std::unique_ptr<DG::Button> returnButton;
    std::unique_ptr<DG::Button> hideButton;

    geomsrv::annotation::DrawList drawList;
    geomsrv::planoverlay::Session overlaySession;
    uint64_t retainedGeneration = 0;
    uint64_t presentedGeneration = 0;
    size_t selectedNode = 0;
    size_t selectedFrame = 0;
    GS::UniString kind;
    bool active = false;
    bool paletteVisible = true;
    bool overlayActive = false;
    bool collapsed = false;
    previewpanel::CanvasInputState inputState;
    previewpanel::PreviewPlanCamera planCamera;
    void* capturedWindow = nullptr;
    bool planFitPending = false;
    bool planCameraSelectionValid = false;
    int overlayOpacityPercent = 55;
    size_t planCameraNode = 0;
    size_t planCameraFrame = 0;
    uint64_t freshAfterGeneration = 0;
    previewpanel::HostState host;
};

} // namespace evp

#endif
