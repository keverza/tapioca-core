#include "Fixture.hpp"
#include "Reload.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace pg = tapioca::playground;
namespace fs = std::filesystem;
void Require (bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error (message);
}
std::string Frame (pg::Module& module, std::string state, float dpi = 1, std::string* text = nullptr)
{
    auto& io = ImGui::GetIO ();
    io.DisplaySize = { 1500, 950 };
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame ();
    const TapiocaUiFrame frame { 1500, 950, dpi, "headless test" };
    if (text)
        ImGui::LogToBuffer ();
    TapiocaUiResult result {};
    Require (module.Draw (frame, result), "DLL draw failed");
    if (text) {
        *text = ImGui::GetCurrentContext ()->LogBuffer.c_str ();
        ImGui::LogFinish ();
    }
    if (result.stateJson) {
        state = result.stateJson;
        std::free (result.stateJson);
    }
    ImGui::Render ();
    Require (ImGui::GetDrawData ()->TotalVtxCount > 0, "DLL and host do not share a drawable context");
    return state;
}

void VerifyCaptureFrame (pg::Module& module, std::string state)
{
    auto& io = ImGui::GetIO ();
    const auto click = [&] (ImVec2 point) {
        io.AddMousePosEvent (point.x, point.y);
        state = Frame (module, state);
        io.AddMouseButtonEvent (0, true);
        state = Frame (module, state);
        io.AddMouseButtonEvent (0, false);
        state = Frame (module, state);
    };
    const auto open = [&] {
        const auto* entry = ImGui::FindWindowByName ("##capture-entry");
        Require (entry && entry->Active, "Vision screenshot entry is missing");
        const ImVec2 start = entry->DC.CursorStartPos;
        click ({ start.x + 12, start.y + 8 });
        state = Frame (module, state);
        state = Frame (module, state);
        const auto* bar = ImGui::FindWindowByName ("##capture-bar");
        Require (bar && bar->Active, "Vision screenshot entry did not open framing");
        Require (!ImGui::FindWindowByName ("###tapioca.dock")->Active, "Framing left the dock visible");
        Require (!ImGui::FindWindowByName ("Tapioca UI Playground")->Active, "Framing left the host visible");
        return bar->DC.CursorPosPrevLine;
    };
    state = Frame (module, state);
    state = Frame (module, state);
    ImVec2 end = open ();
    float buttonHeight = ImGui::GetFontSize () + 2.0f * ImGui::GetStyle ().FramePadding.y;
    click ({ end.x - 10.0f, end.y + buttonHeight * 0.5f });
    Require (state.find ("Mock screenshot request recorded") == std::string::npos, "Cancel requested a screenshot");
    state = Frame (module, state);
    state = Frame (module, state);
    end = open ();
    const auto& style = ImGui::GetStyle ();
    const float cancelWidth = ImGui::CalcTextSize ("Cancel").x + 2.0f * style.FramePadding.x;
    click ({ end.x - cancelWidth - style.ItemSpacing.x - 10.0f, end.y + buttonHeight * 0.5f });
    Require (state.find ("Mock screenshot request recorded") != std::string::npos, "Capture did not request a shot");
    state = Frame (module, state);
    Require (ImGui::FindWindowByName ("###tapioca.dock")->Active, "Capture did not restore the dock");
}

int main (int argc, char** argv)
{
    try {
        Require (argc == 2, "Pass fixtures directory");
        pg::json::JsonArray fixtures;
        size_t editors = 0;
        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator (argv[1]))
            if (entry.path ().extension () == ".json")
                paths.push_back (entry.path ());
        std::sort (paths.begin (), paths.end ());
        for (const auto& path : paths) {
            auto parsed = pg::json::Parse (pg::ReadText (path));
            Require (parsed.ok, "Fixture parse failed");
            // A virtual editor dump is wrapped as the host wraps it (Host.cpp FixtureOf).
            if (pg::IsEditorDump (parsed.value)) {
                auto stem = path.stem ().string ();
                parsed.value =
                    pg::FixtureFromDump (parsed.value, stem.rfind ("editor-", 0) == 0 ? stem.substr (7) : stem);
            }
            pg::FixtureModel model;
            std::string error;
            Require (pg::DecodeFixture (parsed.value, model, error), "Fixture decode failed");
            editors += model.panel == "hudeditor";
            if (model.panel == "hudprogramme") {
                if (model.variant == "valid-populated")
                    Require (model.programme == pg::av::floorprogramme::Default (),
                             "Default fixture drifted from production");
                if (model.variant == "invalid" || model.variant == "empty")
                    Require (!pg::av::floorprogramme::Problem (model.programme).empty (),
                             "Invalid programme fixture unexpectedly valid");
                else
                    Require (pg::av::floorprogramme::Valid (model.programme),
                             "Valid programme fixture rejected by production");
            }
            pg::FixtureModel roundtrip;
            Require (pg::DecodeFixture (pg::EncodeFixture (model), roundtrip, error), "Fixture roundtrip failed");
            Require (pg::json::Write (pg::EncodeFixture (model)) == pg::json::Write (pg::EncodeFixture (roundtrip)),
                     "Fixture roundtrip changed data");
            fixtures.push_back (parsed.value);
        }
        Require (fixtures.size () == 14 + editors, "Expected all seven variants for both panels");
        // #191 U0: one per Inspector context, and the #188 dump.
        Require (editors == 12, "Expected 12 virtual editor fixtures");
        // The Programme interaction below drives the first Programme fixture; editor fixtures sort first.
        const int programme = int (editors);
        Require (pg::Text (fixtures[size_t (programme)], "panel") == "hudprogramme",
                 "Programme fixtures not after editor fixtures");
        auto document = pg::Value::Object ({ { "version", pg::Value::Integer (1) },
                                             { "nativeMode", pg::Value::Bool (false) },
                                             { "fixtures", pg::Value::Array (fixtures) },
                                             { "selected", pg::Value::Integer (0) } });
        IMGUI_CHECKVERSION ();
        ImGui::CreateContext ();
        ImGui::GetIO ().IniFilename = nullptr;
        ImGui::GetIO ().LogFilename = nullptr;
        ImGui::GetIO ().Fonts->AddFontDefault ();
        unsigned char* pixels;
        int width, height;
        ImGui::GetIO ().Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
        {
            pg::Module module;
            std::string error;
            wchar_t executable[32768] {};
            GetModuleFileNameW (nullptr, executable, DWORD (std::size (executable)));
            const auto dll = fs::path (executable).parent_path () / "TapiocaUIPlaygroundUI.dll";
            std::string lastState;
            for (int selected = -1; selected < int (fixtures.size ()); ++selected) {
                pg::Put (document, "selected", pg::Value::Integer (selected));
                for (int density = 0; density < 4; ++density) {
                    pg::Put (document, "density", pg::Value::Integer (density));
                    for (bool dark : { false, true }) {
                        pg::Put (document, "dark", pg::Value::Bool (dark));
                        auto state = pg::json::Write (document, 0);
                        Require (module.Swap (dll, state, error), "Module create/swap failed");
                        state = Frame (module, state);
                        state = Frame (module, state);
                        lastState = state;
                        const auto parsed = pg::json::Parse (state);
                        Require (parsed.ok && pg::Number (parsed.value, "selected", -2) == selected,
                                 "Selection lost on draw");
                        if (selected >= 0 && pg::Text (fixtures[size_t (selected)], "panel") == "hudeditor") {
                            Require (pg::Number (parsed.value, "editorLoaded", -1) == selected,
                                     "Virtual editor fixture not loaded");
                            for (const auto& event : *pg::Member (parsed.value, "events").AsArray ()) {
                                std::string text;
                                event.AsString (text);
                                if (text.find (" rejected: ") != std::string::npos)
                                    throw std::runtime_error (text);
                            }
                        }
                    }
                }
            }
            {
                auto native = pg::json::Parse (lastState).value;
                pg::Put (native, "selected", pg::Value::Integer (programme));
                pg::Put (native, "nativeMode", pg::Value::Bool (true));
                pg::Put (native, "nativeRequest", pg::Value::String ("@analysis"));
                pg::Put (native, "density", pg::Value::Integer (1));
                pg::Put (native, "dpi", pg::Value::Double (1.0));
                auto state = pg::json::Write (native, 0);
                Require (module.Swap (dll, state, error), "Capture fixture restore failed");
                VerifyCaptureFrame (module, state);
            }
            // #191 U2 / #188: the dump's refusal is in the native editor's command bar, unscrolled.
            {
                int refused = -1;
                for (size_t i = 0; i < fixtures.size (); ++i)
                    if (pg::Text (fixtures[i], "variant") == "boolean-refused")
                        refused = int (i);
                Require (refused >= 0, "No boolean-refused editor fixture");
                auto native = pg::json::Parse (lastState).value;
                pg::Put (native, "selected", pg::Value::Integer (refused));
                pg::Put (native, "nativeMode", pg::Value::Bool (true));
                pg::Put (native, "nativeRequest", pg::Value::String ("@editor"));
                auto shown = pg::json::Write (native, 0);
                Require (module.Swap (dll, shown, error), "Refused fixture restore failed");
                shown = Frame (module, shown);
                shown = Frame (module, shown);
                std::string rendered;
                Frame (module, shown, 1, &rendered);
                if (rendered.find ("Refused") == std::string::npos ||
                    rendered.find ("PropertyLineOutside") == std::string::npos)
                    throw std::runtime_error ("The #188 refusal is not in the command bar:\n" + rendered);
            }
            // #191: the region fixture's role picker is a grid of buttons, three across the page, a
            // two-word role on two lines; the editor page's line is the editor's command line.
            {
                int region = -1;
                for (size_t i = 0; i < fixtures.size (); ++i)
                    if (pg::Text (fixtures[i], "variant") == "region")
                        region = int (i);
                Require (region >= 0, "No region editor fixture");
                auto native = pg::json::Parse (lastState).value;
                pg::Put (native, "selected", pg::Value::Integer (region));
                pg::Put (native, "nativeMode", pg::Value::Bool (true));
                pg::Put (native, "nativeRequest", pg::Value::String ("@editor"));
                auto shown = pg::json::Write (native, 0);
                Require (module.Swap (dll, shown, error), "Region fixture restore failed");
                shown = Frame (module, shown);
                shown = Frame (module, shown);
                std::string rendered;
                Frame (module, shown, 1, &rendered);
                if (rendered.find ("Use selected lines as:") == std::string::npos ||
                    rendered.find ("boundary") == std::string::npos ||
                    rendered.find ("Region boundary") != std::string::npos)
                    throw std::runtime_error ("The region's roles are not a two-line button grid:\n" + rendered);
                if (rendered.find ("Command: line") == std::string::npos)
                    throw std::runtime_error ("The editor page's line is not its command line:\n" + rendered);
                bool full = false;
                auto& tables = ImGui::GetCurrentContext ()->Tables;
                for (int i = 0; i < tables.GetMapSize (); ++i)
                    if (const auto* table = tables.TryGetMapData (i);
                        table && table->ColumnsCount == 3 && table->OuterWindow &&
                        table->OuterRect.GetWidth () >= table->OuterWindow->WorkRect.GetWidth () - 2.0f)
                        full = true;
                Require (full, "The role grid does not span the page");
            }
            auto state = lastState;
            const auto generation = module.Generation ();
            Require (!module.Swap (dll, "{\"version\":999}", error), "Bad state candidate should fail Create");
            Require (!module.Swap (dll.parent_path () / "missing.dll", state, error), "Missing candidate accepted");
            Require (!module.Swap (dll.parent_path () / "PlaygroundRejectedModule.dll", state, error),
                     "Mismatched ABI candidate accepted");
            Require (module.Generation () == generation, "Failed candidate replaced working module");
            state = Frame (module, state);
            // Drive a real production Programme drag through host input events.
            document = pg::json::Parse (state).value;
            pg::Put (document, "selected", pg::Value::Integer (programme));
            pg::Put (document, "density", pg::Value::Integer (1));
            state = pg::json::Write (document, 0);
            Require (module.Swap (dll, state, error), "Interaction fixture load failed");
            state = Frame (module, state);
            state = Frame (module, state);
            ImGuiWindow* preview = nullptr;
            for (auto* window : ImGui::GetCurrentContext ()->Windows)
                // The preview itself, not a child of it ("...production-preview.../<id>"): #191 U5's
                // editor Outliner views are child windows that outlive their frame.
                if (const char* at = std::strstr (window->Name, "production-preview"); at && !std::strchr (at, '/'))
                    preview = window;
            Require (preview != nullptr, "Production preview window missing");
            const auto* table = ImGui::GetCurrentContext ()->Tables.GetByKey (preview->GetID ("##massing.programme"));
            Require (table != nullptr, "Production Programme table missing");
            const ImVec2 point { table->Columns[1].WorkMinX + 15,
                                 table->OuterRect.Max.y - ImGui::GetFrameHeight () / 2 };
            auto& io = ImGui::GetIO ();
            io.AddMousePosEvent (point.x, point.y);
            state = Frame (module, state);
            io.AddMouseButtonEvent (0, true);
            state = Frame (module, state);
            io.AddMousePosEvent (point.x + 40, point.y);
            state = Frame (module, state);
            io.AddMouseButtonEvent (0, false);
            state = Frame (module, state);
            const auto edited = pg::json::Parse (state).value;
            const auto& first = (*pg::Member (edited, "fixtures").AsArray ())[size_t (programme)];
            pg::FixtureModel model;
            Require (pg::DecodeFixture (first, model, error), "Edited fixture unreadable");
            Require (model.programme.types[0].rooms != 2.5,
                     "Host mouse input did not edit the DLL's production control");
            Require (model.programme == model.saved, "Production Save outcome not drained by mock owner");
            Require (pg::Member (edited, "events").AsArray () && !pg::Member (edited, "events").AsArray ()->empty (),
                     "No edit events logged");
            for (int i = 0; i < 10; ++i) {
                Require (module.Swap (dll, state, error), "Consecutive reload failed");
                const auto after = Frame (module, state);
                if (pg::json::Write (pg::json::Parse (state).value) !=
                    pg::json::Write (pg::json::Parse (after).value)) {
                    std::ofstream ("reload-before.json") << pg::json::Write (pg::json::Parse (state).value);
                    std::ofstream ("reload-after.json") << pg::json::Write (pg::json::Parse (after).value);
                }
                Require (pg::json::Write (pg::json::Parse (state).value) ==
                             pg::json::Write (pg::json::Parse (after).value),
                         "State changed across reload");
                state = after;
            }
            document = pg::json::Parse (state).value;
            pg::Put (document, "nativeMode", pg::Value::Bool (true));
            for (const auto* workflow : { "@massing", "@analysis", "@editor" }) {
                pg::Put (document, "nativeRequest", pg::Value::String (workflow));
                state = pg::json::Write (document, 0);
                Require (module.Swap (dll, state, error), "Native workflow restore failed");
                state = Frame (module, state);
                state = Frame (module, state);
                const auto* panel = ImGui::FindWindowByName ("###playground.native.hud");
                Require (panel && std::abs (panel->Size.x - 320) <= 1, "Native width differs from actual 320px shell");
                const auto before = pg::json::Parse (state).value;
                Require (module.Swap (dll, state, error), "Native workflow reload failed");
                state = Frame (module, state);
                state = Frame (module, state);
                Require (pg::json::Write (before) == pg::json::Write (pg::json::Parse (state).value),
                         "Native workflow state lost across reload");
                document = pg::json::Parse (state).value;
            }
            pg::Put (document, "nativeRequest", pg::Value::String ("@analysis"));
            for (int tab = 0; tab < 4; ++tab) {
                auto native = pg::Member (document, "native");
                auto values = pg::Member (native, "values");
                pg::Put (values, "analysisTab", pg::Value::Integer (tab));
                pg::Put (native, "values", values);
                pg::Put (document, "native", native);
                state = pg::json::Write (document, 0);
                Require (module.Swap (dll, state, error), "Analysis tab fixture restore failed");
                state = Frame (module, state);
                state = Frame (module, state);
                std::string rendered;
                state = Frame (module, state, 1, &rendered);
                // Visibility draws its run button through AnalysisRunControl, whose label the text log cannot see.
                const char* expected[] = { "Run sun study", "square cells", "Point XYZ (m)", "Show viewpoint" };
                if (rendered.find (expected[tab]) == std::string::npos)
                    throw std::runtime_error ("Analysis tab " + std::to_string (tab) + " body was not rendered: no \"" +
                                              expected[tab] + "\" in:\n" + rendered);
                const auto answer = pg::json::Parse (state).value;
                Require (pg::Number (pg::Member (pg::Member (answer, "native"), "values"), "analysisTab", -1) == tab,
                         "Real analysis sub-tab did not render");
            }
            // Edited production controls and imported-path/profile data survive the ABI snapshot.
            document = pg::json::Parse (state).value;
            auto native = pg::Member (document, "native");
            auto values = pg::Member (native, "values");
            pg::Put (values, "hud.sunRunGrid", pg::Value::Double (2.75));
            pg::Put (values, "point.options.radius", pg::Value::Double (42.5));
            pg::Put (values, "editor.snapEnabled", pg::Value::Bool (false));
            pg::Put (
                values, "path",
                pg::json::Parse (R"({"guid":"fixture-path","points":[[0,0,0],[10,0,0]],"along":[0,10],"closed":false})")
                    .value);
            pg::Put (values, "profile", pg::json::Parse ("[[0,1.6,1,0],[10,2.6,1,0]]").value);
            pg::Put (native, "values", values);
            pg::Put (document, "native", native);
            state = pg::json::Write (document, 0);
            Require (module.Swap (dll, state, error), "Edited native controls rejected");
            state = Frame (module, state);
            // Keep an owning JSON value: Member returns references into it.
            const auto changedDocument = pg::json::Parse (state).value;
            const auto& changedValues = pg::Member (pg::Member (changedDocument, "native"), "values");
            Require (pg::Number (changedValues, "hud.sunRunGrid") == 2.75 &&
                         pg::Number (changedValues, "point.options.radius") == 42.5 &&
                         !pg::Flag (changedValues, "editor.snapEnabled", true),
                     "Native edited values were reset");
            Require (pg::json::Write (pg::Member (changedValues, "profile")) ==
                         pg::json::Write (pg::Member (values, "profile")),
                     "Analysis path profile lost across restore");
            const auto activeGeneration = module.Generation ();
            auto invalid = changedDocument;
            native = pg::Member (invalid, "native");
            values = pg::Member (native, "values");
            pg::Put (values, "analysisTab", pg::Value::Integer (99));
            pg::Put (native, "values", values);
            pg::Put (invalid, "native", native);
            Require (!module.Swap (dll, pg::json::Write (invalid, 0), error) &&
                         module.Generation () == activeGeneration,
                     "Invalid workflow state replaced working module");
            for (float dpi : { 1.0f, 1.5f, 2.0f }) {
                state = Frame (module, state, dpi);
                state = Frame (module, state, dpi);
                const auto* panel = ImGui::FindWindowByName ("###playground.native.hud");
                Require (panel && std::abs (panel->Size.x - 320 * dpi) <= 1, "Production native DPI width mismatch");
            }
            std::cout << editors << " editor fixtures load; " << fixtures.size () << " fixtures roundtrip; "
                      << (fixtures.size () + 1) * 8
                      << " panel/theme/density cases; real Programme input/save; 10 "
                         "consecutive reloads; native workflows, four Analysis bodies, DPI width and edited controls; "
                         "candidate "
                         "rollback passed.\n";
        }
        ImGui::DestroyContext ();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
