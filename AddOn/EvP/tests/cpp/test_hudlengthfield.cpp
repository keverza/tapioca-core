// #109 / LD-09: the editor's length fields show, drag and read the project's length unit while the
// session keeps metres; bounds, speed and precision are defined once in metres and converted.
#include "ArchViz/HudLengthField.hpp"
#include "Edit/EditDisplayUnits.hpp"
#include <gtest/gtest.h>
#include <imgui.h>

namespace ed = geomsrv::edit;
namespace eh = geomsrv::archviz::edithud;

namespace {
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

ed::DisplayUnits Project (ed::DisplayUnits::Length length, int decimals)
{
    ed::DisplayUnits u;
    u.fromProject = true;
    u.length = length;
    u.lengthDecimals = decimals;
    return u;
}

struct Context {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    Context ()
    {
        ImGuiIO& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 800, 600 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~Context ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
};
} // namespace

TEST (HudLengthField, TheFieldIsDefinedInMetresAndShownInTheProjectsUnit109)
{
    const eh::LengthField grid { "Grid spacing", 0.01, 100.0, 0.05, 2 };
    // Defaults (no project read yet): metres, exactly as the panels showed them before.
    auto m = eh::ShowLength (ed::DisplayUnits {}, grid);
    EXPECT_EQ (m.format, "%.2f m");
    EXPECT_DOUBLE_EQ (m.low, 0.01);
    EXPECT_DOUBLE_EQ (m.high, 100.0);
    EXPECT_DOUBLE_EQ (m.speed, 0.05);

    auto mm = eh::ShowLength (Project (ed::DisplayUnits::Length::Millimetre, 0), grid);
    EXPECT_EQ (mm.format, "%.0f mm");
    EXPECT_DOUBLE_EQ (mm.low, 10.0);
    EXPECT_DOUBLE_EQ (mm.high, 100000.0);
    EXPECT_DOUBLE_EQ (mm.speed, 50.0);

    // A field's own precision is carried into the unit: 3 decimals of a metre are 1 of a
    // centimetre; the project's decimals are a floor, never a ceiling.
    const eh::LengthField fine { "Point reduction", 0.0, 0.5, 0.0, 3 };
    EXPECT_EQ (eh::ShowLength (Project (ed::DisplayUnits::Length::Centimetre, 0), fine).format, "%.1f cm");
    EXPECT_EQ (eh::ShowLength (Project (ed::DisplayUnits::Length::Centimetre, 2), fine).format, "%.2f cm");
    EXPECT_EQ (eh::ShowLength (Project (ed::DisplayUnits::Length::Metre, 4), fine).format, "%.4f m");

    EXPECT_EQ (eh::WithLengthUnit (Project (ed::DisplayUnits::Length::Millimetre, 0), "Contour interval"),
               "Contour interval (mm)");
}

// Typed into a millimetre project, stored in metres; the bound is the metre bound converted.
TEST (HudLengthField, TypedInTheProjectsUnitStoredInMetres109)
{
    UnitsGuard units (Project (ed::DisplayUnits::Length::Millimetre, 0));
    Context context;
    ImGuiIO& io = ImGui::GetIO ();
    double metres = 1.0;
    bool changed = false;
    const auto frame = [&] (bool focus, ImGuiKey key, const char* typed) {
        if (key != ImGuiKey_None)
            io.AddKeyEvent (key, true);
        if (typed != nullptr)
            io.AddInputCharactersUTF8 (typed);
        ImGui::NewFrame ();
        ImGui::Begin ("fields");
        if (focus)
            ImGui::SetKeyboardFocusHere ();
        changed |=
            eh::LengthInput ({ "Spacing", 0.0, 2.0, 0.1, 3 }, metres, 0.0, 0.0, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::End ();
        ImGui::Render ();
        if (key != ImGuiKey_None)
            io.AddKeyEvent (key, false);
    };
    frame (true, ImGuiKey_None, nullptr);
    frame (false, ImGuiKey_None, nullptr);
    frame (false, ImGuiKey_None, "1250");
    EXPECT_FALSE (changed) << "typing alone never commits";
    frame (false, ImGuiKey_Enter, nullptr);
    frame (false, ImGuiKey_None, nullptr);
    EXPECT_TRUE (changed);
    EXPECT_DOUBLE_EQ (metres, 1.25);

    changed = false;
    frame (true, ImGuiKey_None, nullptr);
    frame (false, ImGuiKey_None, nullptr);
    frame (false, ImGuiKey_None, "9000");
    frame (false, ImGuiKey_Enter, nullptr);
    frame (false, ImGuiKey_None, nullptr);
    EXPECT_TRUE (changed);
    EXPECT_DOUBLE_EQ (metres, 2.0) << "clamped to the field's 2 m";
}
