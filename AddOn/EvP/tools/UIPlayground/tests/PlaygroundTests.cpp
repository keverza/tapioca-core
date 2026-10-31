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
std::string Frame (pg::Module& module, std::string state)
{
    auto& io = ImGui::GetIO ();
    io.DisplaySize = { 1500, 950 };
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame ();
    const TapiocaUiFrame frame { 1500, 950, 1, "headless test" };
    TapiocaUiResult result {};
    Require (module.Draw (frame, result), "DLL draw failed");
    if (result.stateJson) {
        state = result.stateJson;
        std::free (result.stateJson);
    }
    ImGui::Render ();
    Require (ImGui::GetDrawData ()->TotalVtxCount > 0, "DLL and host do not share a drawable context");
    return state;
}
int main (int argc, char** argv)
{
    try {
        Require (argc == 2, "Pass fixtures directory");
        pg::json::JsonArray fixtures;
        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator (argv[1]))
            if (entry.path ().extension () == ".json")
                paths.push_back (entry.path ());
        std::sort (paths.begin (), paths.end ());
        for (const auto& path : paths) {
            auto parsed = pg::json::Parse (pg::ReadText (path));
            pg::FixtureModel model;
            std::string error;
            Require (parsed.ok && pg::DecodeFixture (parsed.value, model, error), "Fixture decode failed");
            pg::FixtureModel roundtrip;
            Require (pg::DecodeFixture (pg::EncodeFixture (model), roundtrip, error), "Fixture roundtrip failed");
            Require (pg::json::Write (pg::EncodeFixture (model)) == pg::json::Write (pg::EncodeFixture (roundtrip)),
                     "Fixture roundtrip changed data");
            fixtures.push_back (parsed.value);
        }
        Require (fixtures.size () == 14, "Expected all seven variants for both panels");
        auto document = pg::Value::Object ({ { "version", pg::Value::Integer (1) },
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
                    }
                }
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
            pg::Put (document, "selected", pg::Value::Integer (0));
            pg::Put (document, "density", pg::Value::Integer (1));
            state = pg::json::Write (document, 0);
            Require (module.Swap (dll, state, error), "Interaction fixture load failed");
            state = Frame (module, state); state = Frame (module, state);
            ImGuiWindow* preview = nullptr;
            for (auto* window : ImGui::GetCurrentContext ()->Windows)
                if (std::strstr (window->Name, "production-preview")) preview = window;
            Require (preview != nullptr, "Production preview window missing");
            const auto* table = ImGui::GetCurrentContext ()->Tables.GetByKey (preview->GetID ("##massing.programme"));
            Require (table != nullptr, "Production Programme table missing");
            const ImVec2 point { table->Columns[1].WorkMinX + 15, table->OuterRect.Max.y - ImGui::GetFrameHeight () / 2 };
            auto& io = ImGui::GetIO ();
            io.AddMousePosEvent (point.x, point.y); state = Frame (module, state);
            io.AddMouseButtonEvent (0, true); state = Frame (module, state);
            io.AddMousePosEvent (point.x + 40, point.y); state = Frame (module, state);
            io.AddMouseButtonEvent (0, false); state = Frame (module, state);
            const auto edited = pg::json::Parse (state).value;
            const auto& first = (*pg::Member (edited, "fixtures").AsArray ())[0];
            pg::FixtureModel model;
            Require (pg::DecodeFixture (first, model, error), "Edited fixture unreadable");
            Require (model.programme.types[0].rooms != 2.5, "Host mouse input did not edit the DLL's production control");
            Require (model.programme == model.saved, "Production Save outcome not drained by mock owner");
            Require (pg::Member (edited, "events").AsArray () && !pg::Member (edited, "events").AsArray ()->empty (), "No edit events logged");
            for (int i = 0; i < 10; ++i) {
                Require (module.Swap (dll, state, error), "Consecutive reload failed");
                const auto after = Frame (module, state);
                if (pg::json::Write (pg::json::Parse (state).value) != pg::json::Write (pg::json::Parse (after).value)) {
                    std::ofstream ("reload-before.json") << pg::json::Write (pg::json::Parse (state).value);
                    std::ofstream ("reload-after.json") << pg::json::Write (pg::json::Parse (after).value);
                }
                Require (pg::json::Write (pg::json::Parse (state).value) ==
                             pg::json::Write (pg::json::Parse (after).value),
                         "State changed across reload");
                state = after;
            }
            std::cout << "14 fixtures roundtrip; 120 panel/theme/density cases; real Programme input/save; 10 consecutive reloads; candidate "
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
