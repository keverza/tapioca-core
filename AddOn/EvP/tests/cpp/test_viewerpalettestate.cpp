#include "ArchViz/ViewerPaletteState.hpp"

#include <gtest/gtest.h>

using geomsrv::archviz::ViewerPaletteState;

TEST (ViewerPaletteState, ANewSessionCannotRestoreSavedWorkspaceVisibility)
{
    ViewerPaletteState previousSession;
    previousSession.RequestOpen ();
    ASSERT_TRUE (previousSession.CanShow ());
    ViewerPaletteState newSession;
    EXPECT_FALSE (newSession.CanShow ());
    newSession.BeginHostHide (false);
    EXPECT_FALSE (newSession.EndHostHide ());
    newSession.RequestOpen ();
    EXPECT_TRUE (newSession.CanShow ());
}

TEST (ViewerPaletteState, AProjectReplacementClosesIntentBeforeTheHostRestoresPalettes)
{
    ViewerPaletteState state;
    state.RequestOpen ();
    state.BeginHostHide (true);
    state.RequestClose (); // New/Open/Close reset, even within a project-load hide
    EXPECT_FALSE (state.EndHostHide ());
    EXPECT_FALSE (state.CanShow ());
    state.RequestOpen ();
    EXPECT_TRUE (state.CanShow ());
}

TEST (ViewerPaletteState, AClosedInstanceDoesNotRestoreAfterAViewSwitch)
{
    ViewerPaletteState state;
    state.RequestOpen ();
    state.RequestClose ();
    state.BeginHostHide (false); // schedule, then back to plan or 3D
    EXPECT_FALSE (state.EndHostHide ());
    EXPECT_FALSE (state.CanShow ());
}

TEST (ViewerPaletteState, HostHideRestoresAnOpenVisibleViewerOnce)
{
    ViewerPaletteState state;
    state.RequestOpen ();
    state.BeginHostHide (true);
    EXPECT_FALSE (state.CanShow ());
    EXPECT_TRUE (state.EndHostHide ());
    EXPECT_TRUE (state.CanShow ());
    EXPECT_FALSE (state.EndHostHide ());
}

TEST (ViewerPaletteState, ClosingWhileHostHiddenCancelsRestore)
{
    ViewerPaletteState state;
    state.RequestOpen ();
    state.BeginHostHide (true);
    state.RequestClose ();
    EXPECT_FALSE (state.EndHostHide ());
    EXPECT_FALSE (state.CanShow ());
}

TEST (ViewerPaletteState, NestedHidesRestoreOnlyAfterTheLastEnd)
{
    ViewerPaletteState state;
    state.RequestOpen ();
    state.BeginHostHide (true);
    state.BeginHostHide (false);
    EXPECT_FALSE (state.EndHostHide ());
    EXPECT_FALSE (state.CanShow ());
    EXPECT_TRUE (state.EndHostHide ());
    EXPECT_TRUE (state.CanShow ());
}

TEST (ViewerPaletteState, ClosingDuringNestedHidesCannotBeUndoneByAnEnd)
{
    ViewerPaletteState state;
    state.RequestOpen ();
    state.BeginHostHide (true);
    state.BeginHostHide (false);
    state.RequestClose ();
    EXPECT_FALSE (state.EndHostHide ());
    EXPECT_FALSE (state.EndHostHide ());
    state.BeginHostHide (false);
    EXPECT_FALSE (state.EndHostHide ());
}

TEST (ViewerPaletteState, AnInvisibleOpenPanelIsNotRestoredSpeculatively)
{
    ViewerPaletteState state;
    state.RequestOpen ();
    state.BeginHostHide (false);
    EXPECT_FALSE (state.EndHostHide ());
}

TEST (ViewerPaletteState, AnExplicitReopenDuringHostHideWaitsForItsEnd)
{
    ViewerPaletteState state;
    state.BeginHostHide (false);
    state.RequestClose ();
    state.RequestOpen ();
    EXPECT_FALSE (state.CanShow ());
    EXPECT_TRUE (state.EndHostHide ());
    EXPECT_TRUE (state.CanShow ());
}

TEST (ViewerPaletteState, RepeatedClosedViewSwitchesNeverShowThePanel)
{
    ViewerPaletteState state;
    for (int change = 0; change < 20; ++change) {
        state.BeginHostHide (false);
        EXPECT_FALSE (state.EndHostHide ());
    }
    state.RequestOpen ();
    EXPECT_TRUE (state.CanShow ());
}
