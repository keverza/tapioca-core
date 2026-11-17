// #109: the editor's display units -- the project's working units, the document always metres.
// Lengths are shown and typed in the project's length unit; angles, slopes and factors are not.
#include "ArchViz/EditLayers.hpp"
#include "Edit/EditDisplayUnits.hpp"
#include "Edit/EditTracker.hpp"
#include <gtest/gtest.h>

namespace ed = geomsrv::edit;

namespace {
// The snapshot is process-wide: a test that sets it puts it back.
struct UnitsGuard {
    ed::DisplayUnits before = ed::CurrentDisplayUnits ();
    explicit UnitsGuard (const ed::DisplayUnits& units)
    {
        ed::SetDisplayUnits (units);
    }
    ~UnitsGuard ()
    {
        ed::SetDisplayUnits (before);
    }
};

ed::DisplayUnits Centimetres (int decimals = 1)
{
    ed::DisplayUnits u;
    u.fromProject = true;
    u.length = ed::DisplayUnits::Length::Centimetre;
    u.lengthDecimals = decimals;
    return u;
}

ed::TrackerKey Char (char c)
{
    ed::TrackerKey k;
    k.kind = ed::TrackerKey::Kind::Char;
    k.c = c;
    return k;
}
ed::TrackerKey Key (ed::TrackerKey::Kind kind)
{
    ed::TrackerKey k;
    k.kind = kind;
    return k;
}
} // namespace

TEST (EditDisplayUnits, TheDefaultsAreMetresAsTheEditorAlwaysWrote)
{
    const ed::DisplayUnits u;
    EXPECT_FALSE (u.fromProject);
    EXPECT_EQ (ed::FormatLength (u, 12.5), "12.50 m");
    EXPECT_EQ (ed::FormatLength (u, 1.25, true), "+1.25 m");
    EXPECT_EQ (ed::FormatLength (u, -0.5, true), "-0.50 m");
    EXPECT_EQ (ed::FormatArea (u, 100), "100.00 m2");
    EXPECT_EQ (ed::FormatVolume (u, 2.5, false), "2.50");
    EXPECT_DOUBLE_EQ (ed::TypedToDocument (u, ed::Quantity::Length, 3.25), 3.25);
}

TEST (EditDisplayUnits, LengthsAreShownAndTypedInTheProjectUnitAndOthersAreNot)
{
    auto u = Centimetres (0);
    EXPECT_EQ (ed::FormatLength (u, 12.5), "1250 cm");
    EXPECT_EQ (ed::FormatLength (u, 0.256, true), "+26 cm");
    EXPECT_DOUBLE_EQ (ed::TypedToDocument (u, ed::Quantity::Width, 250), 2.5);
    EXPECT_DOUBLE_EQ (ed::TypedToDocument (u, ed::Quantity::Elevation, -15), -0.15);
    EXPECT_DOUBLE_EQ (ed::TypedToDocument (u, ed::Quantity::Slope, 2), 2) << "a slope is a percentage";
    EXPECT_DOUBLE_EQ (ed::TypedToDocument (u, ed::Quantity::Angle, 30), 30);
    EXPECT_DOUBLE_EQ (ed::TypedToDocument (u, ed::Quantity::Factor, 2), 2);
    u.length = ed::DisplayUnits::Length::Millimetre;
    u.lengthDecimals = 0;
    EXPECT_EQ (ed::FormatLength (u, 1.2346), "1235 mm");
    EXPECT_DOUBLE_EQ (ed::LengthToMetres (u, ed::MetresToLength (u, 7.125)), 7.125);
    u.length = ed::DisplayUnits::Length::Kilometre;
    u.lengthDecimals = 3;
    EXPECT_EQ (ed::FormatLength (u, 1500), "1.500 km");
    u.lengthDecimals = 9;
    EXPECT_EQ (ed::FormatLength (u, 1500), "1.5000 km") << "Archicad keeps 0..4 decimals";

    ed::DisplayUnits big;
    big.area = ed::DisplayUnits::Area::SquareCentimetre;
    big.volume = ed::DisplayUnits::Volume::Litre;
    big.areaDecimals = big.volumeDecimals = 0;
    EXPECT_EQ (ed::FormatArea (big, 1.5), "15000 cm2");
    EXPECT_EQ (ed::FormatVolume (big, 2.5), "2500 L");
}

TEST (EditDisplayUnits, LabelsAndTheTrackerFollowTheSnapshot)
{
    EXPECT_EQ (geomsrv::archviz::editlayers::FormatValue (ed::Quantity::Length, 2.5), "2.50 m");
    {
        UnitsGuard cm (Centimetres ());
        EXPECT_EQ (ed::CurrentDisplayUnits (), Centimetres ());
        EXPECT_EQ (geomsrv::archviz::editlayers::FormatValue (ed::Quantity::Length, 2.5), "250.0 cm");
        EXPECT_EQ (geomsrv::archviz::editlayers::FormatValue (ed::Quantity::Elevation, 0.3), "+30.0 cm");
        EXPECT_EQ (geomsrv::archviz::editlayers::FormatValue (ed::Quantity::Slope, 2.0), "2.0 %");

        // Typed "125" for a length is 1.25 m; Tab locks it in metres too. A slope stays as typed.
        ed::Tracker t;
        t.Show ({ { ed::Quantity::Length, 1.0 }, { ed::Quantity::Slope, 0.0 } });
        for (const char c : std::string ("125"))
            t.Key (Char (c));
        auto locked = t.Key (Key (ed::TrackerKey::Kind::Tab));
        EXPECT_EQ (locked.outcome, ed::Tracker::Outcome::Locked);
        EXPECT_DOUBLE_EQ (locked.value, 1.25);
        EXPECT_DOUBLE_EQ (t.Items ()[0].lockedValue, 1.25);
        t.Key (Char ('4'));
        const auto slope = t.Key (Key (ed::TrackerKey::Kind::Enter));
        EXPECT_EQ (slope.outcome, ed::Tracker::Outcome::Commit);
        EXPECT_DOUBLE_EQ (slope.value, 4.0);
    }
    EXPECT_EQ (geomsrv::archviz::editlayers::FormatValue (ed::Quantity::Length, 2.5), "2.50 m") << "put back";
}
