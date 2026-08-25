#include "Palette/PreviewPanelState.hpp"
#include "Preview/PreviewRuntimeState.hpp"

#include <gtest/gtest.h>

using evp::previewpanel::Host;

TEST (PreviewPanelState, ExpandedCanvasUsesFullWidthAndFixedHeight)
{
    const auto layout = evp::previewpanel::BuildLayout (10, 410, 600, true, false);

    EXPECT_GT (layout.height, 118);
    EXPECT_LT (layout.height, 400);
    EXPECT_TRUE (layout.showCanvas);
    EXPECT_EQ (layout.canvas.left, 10);
    EXPECT_EQ (layout.canvas.right, 410);
    EXPECT_EQ (layout.canvas.Width (), 400);
    EXPECT_EQ (layout.canvas.Height (), 224);
    EXPECT_LE (layout.canvas.bottom, layout.nodeSelector.top);
    EXPECT_LE (layout.nodeSelector.bottom, layout.overlayButton.top);
}

TEST (PreviewPanelState, DisabledAndExternalLayoutsCollapseTheCanvas)
{
    const auto disabled = evp::previewpanel::BuildLayout (0, 300, 500, false, false);
    const auto external = evp::previewpanel::BuildLayout (0, 300, 500, true, true);

    EXPECT_EQ (disabled.height, 22);
    EXPECT_FALSE (disabled.showCanvas);
    EXPECT_EQ (disabled.enableControl.Width (), 300);
    EXPECT_FALSE (external.showCanvas);
    EXPECT_GT (external.height, disabled.height);
    EXPECT_LT (external.height, 118);
}

TEST (PreviewPanelState, ReturnExistsOnlyForAnExternalTargetStartingOrActive)
{
    evp::previewpanel::HostState state;
    EXPECT_FALSE (state.ExternalStartingOrActive ());

    state.current = Host::Band;
    state.target = Host::Overlay;
    state.transition = true;
    EXPECT_TRUE (state.ExternalStartingOrActive ());
    EXPECT_TRUE (state.CanvasCollapsed ());

    state.current = Host::Overlay;
    state.transition = false;
    EXPECT_TRUE (state.ExternalStartingOrActive ());

    state.target = Host::Band;
    state.transition = true;
    EXPECT_FALSE (state.ExternalStartingOrActive ());
}

TEST (PreviewRuntimeState, OneStateGatesEveryPresentation)
{
    auto& state = evp::preview::PreviewRuntimeState::Get ();
    const bool original = state.IsEnabled ();
    state.SetEnabled (false);
    EXPECT_FALSE (state.IsEnabled ());
    state.SetEnabled (true);
    EXPECT_TRUE (state.IsEnabled ());
    state.SetEnabled (original);
}

TEST (PreviewPanelInput, RoutesOnlyWhileAvailableAndInside)
{
    evp::previewpanel::CanvasInputState input;
    EXPECT_FALSE (input.CanRoutePointer ());
    EXPECT_FALSE (input.Press (1));

    input.SetAvailable (true);
    input.SetPointerInside (true);
    EXPECT_TRUE (input.CanRoutePointer ());
    EXPECT_TRUE (input.Press (4));
    EXPECT_TRUE (input.IsDragging ());
    EXPECT_TRUE (input.Release (4));
    EXPECT_FALSE (input.IsDragging ());
}

TEST (PreviewPanelInput, ExitAndLifecycleDisableReleaseEveryButton)
{
    evp::previewpanel::CanvasInputState input;
    input.SetAvailable (true);
    input.SetPointerInside (true);
    ASSERT_TRUE (input.Press (1 | 2 | 4));

    EXPECT_TRUE (input.ReleaseAll ());
    EXPECT_FALSE (input.CanRoutePointer ());
    EXPECT_FALSE (input.IsDragging ());

    input.SetPointerInside (true);
    ASSERT_TRUE (input.Press (4));
    input.SetAvailable (false);
    EXPECT_FALSE (input.CanRoutePointer ());
    EXPECT_FALSE (input.IsDragging ());
}
