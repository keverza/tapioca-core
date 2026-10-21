// ArchViz/OverlayHudControls -- the HUD's controls: the engine's (OverlayHudEngine.hpp)
// checkboxes, sliders, dropdowns, buttons and tab bar, and the values it holds for them.
// See OverlayHud.hpp's notes on what the add-on holds.

#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/OverlayHudItems.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace geomsrv {
namespace archviz {
namespace overlayhud {

namespace {

// `given` is what the layer says now. ⚠️ THE CALLER'S VALUE WINS ONLY WHEN IT CHANGES: a
// layer set again with what it said before keeps what the user set; one that says
// something new -- Python changed its mind -- sets it.
void Hold (State::Panel& state, const std::string& id, double given)
{
    const auto made = state.values.try_emplace (id);
    State::Held& held = made.first->second;
    if (made.second || given != held.sent) {
        held.sent = given;
        held.current = given;
    }
}

} // namespace

std::string TabBarId (const layers::Panel& panel, size_t* first)
{
    for (size_t i = 0; i < panel.items.size (); ++i)
        if (panel.items[i].kind == layers::ItemKind::Tab) {
            if (first != nullptr)
                *first = i;
            return panel.items[i].id.empty () ? std::string ("tabs") : panel.items[i].id;
        }
    return std::string ();
}

void Engine::Impl::Reconcile (const std::vector<const layers::Panel*>& panels, const std::vector<std::string>& keys)
{
    for (size_t p = 0; p < panels.size (); ++p) {
        const layers::Panel& panel = *panels[p];
        PanelState& state = StateOf (keys[p], panel);
        for (const layers::PanelItem& item : panel.items) {
            switch (item.kind) {
                case layers::ItemKind::Checkbox:
                    Hold (state, item.id, item.checked ? 1.0 : 0.0);
                    break;
                case layers::ItemKind::Slider:
                    Hold (state, item.id, (std::min) ((std::max) (item.number, item.min), item.max));
                    break;
                case layers::ItemKind::Ramp:
                    if (!item.colormap.cutoffControl.empty ())
                        Hold (state, item.colormap.cutoffControl, item.colormap.cutoff);
                    if (!item.colormap.greenControl.empty ())
                        Hold (state, item.colormap.greenControl, item.colormap.green);
                    break;
                case layers::ItemKind::Combo:
                case layers::ItemKind::LayoutChoices:
                    Hold (state, item.id, double (item.selected));
                    break;
                case layers::ItemKind::SitePlan:
                    Hold (state, item.id + ":selected", double (item.selected));
                    Hold (state, item.id + ":targetPoint", -1);
                    Hold (state, item.id + ":targetEdge", -1);
                    for (size_t v = 0; v < item.outlineXY.size () / 2; ++v) {
                        const std::string suffix = std::to_string (v);
                        Hold (state, item.id + ":offset:" + suffix, item.setbackDistances[v]);
                        Hold (state, item.id + ":mode:" + suffix, double (item.setbackModes[v]));
                        Hold (state, item.id + ":point:" + suffix,
                              std::find (item.referenceVertices.begin (), item.referenceVertices.end (), v) !=
                                      item.referenceVertices.end ()
                                  ? 1
                                  : 0);
                    }
                    break;
                default:
                    break;
            }
        }
        size_t first = panel.items.size ();
        const std::string bar = TabBarId (panel, &first);
        if (!bar.empty ())
            Hold (state, bar, double (panel.items[first].selected));
    }
}

void Engine::Impl::Control (const layers::Panel& panel, const layers::PanelItem& item, size_t index, PanelState& state,
                            float width, float scale)
{
    const float w = item.widthPixels > 0.0f ? item.widthPixels * scale : width;
    auto said = [&] (const char* kind, double value, std::string text, bool final) {
        changes.push_back ({ kind, key, panel.title, item.id, int32_t (index), value, std::move (text), final });
    };
    if (item.kind == layers::ItemKind::Swatch) {
        const float side = ImGui::GetFrameHeight ();
        if (ImGui::BeginTable ("##swatch.row", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings,
                               { w, 0 })) {
            ImGui::TableSetupColumn ("color", ImGuiTableColumnFlags_WidthFixed, side);
            ImGui::TableSetupColumn ("material", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn ("area", ImGuiTableColumnFlags_WidthFixed,
                                     ImGui::CalcTextSize (item.value.c_str ()).x);
            ImGui::TableNextRow ();
            ImGui::TableSetColumnIndex (0);
            if (ImGui::ColorButton ("##swatch", items::Colour (item.rgba),
                                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop |
                                        ImGuiColorEditFlags_NoAlpha,
                                    { side, side }))
                said ("swatch", 1.0, item.text, true);
            if (ImGui::IsItemHovered ())
                ImGui::SetMouseCursor (ImGuiMouseCursor_Hand);
            if (item.checked)
                ImGui::GetWindowDrawList ()->AddRect (ImGui::GetItemRectMin (), ImGui::GetItemRectMax (),
                                                      items::Packed (panel.accentRgba), 2 * scale, 0, 2 * scale);
            ImGui::TableSetColumnIndex (1);
            ImGui::TextWrapped ("%s", item.text.c_str ());
            ImGui::TableSetColumnIndex (2);
            ImGui::TextUnformatted (item.value.c_str ());
            ImGui::EndTable ();
        }
        return;
    }
    if (item.kind == layers::ItemKind::Button) {
        if (items::Button (item, w))
            said ("button", 1.0, item.text, true);
        return;
    }
    State::Held& held = state.values[item.id]; // Reconcile made it
    switch (item.kind) {
        case layers::ItemKind::Checkbox: {
            bool on = held.current != 0.0;
            if (items::Checkbox (item, on)) {
                held.current = on ? 1.0 : 0.0;
                said ("checkbox", held.current, on ? "on" : "off", true);
            }
            break;
        }
        case layers::ItemKind::Slider: {
            double value = held.current;
            bool released = false;
            const bool moved = items::Slider (panel, item, value, w, released);
            // ⚠️ A DRAG SAYS EVERY VALUE ON THE WAY, NOT FINAL, AND THE RELEASE SAYS THE LAST ONE,
            // FINAL: a script that only acts on the result waits for `final`.
            if (moved) {
                held.current = value;
                said ("slider", value, items::SliderText (item, value), false);
            }
            if (released)
                said ("slider", held.current, items::SliderText (item, held.current), true);
            break;
        }
        case layers::ItemKind::Combo: {
            uint32_t chosen = uint32_t ((std::max) (held.current, 0.0));
            if (items::Combo (panel, item, chosen, w)) {
                held.current = double (chosen);
                said ("combo", held.current, chosen < item.labels.size () ? item.labels[chosen] : std::string (), true);
            }
            break;
        }
        default:
            break;
    }
}

// BOUND BY OVERLAY-INVARIANTS.md: local ImGui diagrams only. Choice visibility
// uses the existing shared HUD state, never a camera, renderer or ACAPI call.
void Engine::Impl::LayoutChoices (const layers::Panel& panel, const layers::PanelItem& item, size_t index,
                                  PanelState& state, float width, float scale)
{
    const float gap = 8.0f * scale;
    const size_t columns = (std::min) (size_t (2), item.choices.size ());
    const float w = (std::max) (80.0f * scale, (width - gap * float (columns - 1)) / float (columns));
    const float h = (item.heightPixels > 0 ? item.heightPixels : 200.0f) * scale;
    auto& held = state.values[item.id];
    for (size_t choiceIndex = 0; choiceIndex < item.choices.size (); ++choiceIndex) {
        if (choiceIndex % columns != 0)
            ImGui::SameLine (0, gap);
        ImGui::PushID (int (choiceIndex));
        const auto& choice = item.choices[choiceIndex];
        const ImVec2 origin = ImGui::GetCursorScreenPos ();
        const ImVec2 end (origin.x + w, origin.y + h);
        if (ImGui::InvisibleButton ("##layout.choice", ImVec2 (w, h))) {
            held.current = double (choiceIndex);
            for (size_t i = 0; i < item.choices.size (); ++i)
                SetLayerShown (*store, item.choices[i].layer, i == choiceIndex);
            changes.push_back (
                { "layoutChoice", key, panel.title, item.id, int32_t (index), held.current, choice.text, true });
        }
        const bool selected = size_t (held.current) == choiceIndex;
        ImDrawList* draw = ImGui::GetWindowDrawList ();
        draw->AddRectFilled (origin, end, selected ? IM_COL32 (230, 241, 255, 255) : IM_COL32 (244, 246, 249, 255),
                             5 * scale);
        draw->AddRect (origin, end, selected ? IM_COL32 (40, 120, 225, 255) : IM_COL32 (170, 180, 190, 255), 5 * scale,
                       0, selected ? 2.0f : 1.0f);
        draw->PushClipRect (origin, end, true);
        draw->AddText (ImVec2 (origin.x + 8 * scale, origin.y + 6 * scale), IM_COL32 (30, 45, 65, 255),
                       choice.text.c_str ());
        const std::string count = std::to_string (choice.count) + " spaces";
        draw->AddText (ImVec2 (origin.x + 8 * scale, end.y - 23 * scale), IM_COL32 (30, 45, 65, 255), count.c_str ());
        bool first = true;
        double minX = 0, maxX = 0, minY = 0, maxY = 0;
        for (const auto& path : choice.paths)
            for (size_t k = 0; k + 1 < path.xy.size (); k += 2) {
                const double x = path.xy[k], y = path.xy[k + 1];
                if (first) {
                    minX = maxX = x;
                    minY = maxY = y;
                    first = false;
                }
                minX = (std::min) (minX, x);
                maxX = (std::max) (maxX, x);
                minY = (std::min) (minY, y);
                maxY = (std::max) (maxY, y);
            }
        const double factor = (std::min) ((w - 16 * scale) / (std::max) (maxX - minX, 1e-9),
                                          (h - 60 * scale) / (std::max) (maxY - minY, 1e-9));
        for (const auto& path : choice.paths) {
            std::vector<ImVec2> points;
            points.reserve (path.xy.size () / 2);
            for (size_t k = 0; k + 1 < path.xy.size (); k += 2)
                points.emplace_back (origin.x + w * 0.5f + float ((path.xy[k] - (minX + maxX) * 0.5) * factor),
                                     origin.y + h * 0.5f - float ((path.xy[k + 1] - (minY + maxY) * 0.5) * factor));
            const uint32_t c = path.rgba;
            draw->AddPolyline (points.data (), int (points.size ()),
                               IM_COL32 ((c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255), ImDrawFlags_Closed,
                               1.0f * scale);
        }
        draw->PopClipRect ();
        ImGui::PopID ();
    }
}

std::vector<Change> TakeRampCutoffs (State& state)
{
    std::vector<Change> pending;
    pending.swap (state.rampCutoffs);
    return pending;
}

std::vector<Change> TakeRampGreens (State& state)
{
    std::vector<Change> pending;
    pending.swap (state.rampGreens);
    return pending;
}

void Engine::Impl::HoverTint (const Hover& hover)
{
    const ImGuiWindow* window = ImGui::GetCurrentContext ()->HoveredWindow;
    tinted = hover.active && !hover.tintModel.empty () && !highlight.active &&
             (window == nullptr || window == ImGui::FindWindowByName ("###tapioca.lockedView"));
}

void Engine::Impl::PointerButtons (const Input& input)
{
    ImGuiIO& io = ImGui::GetIO ();
    if (input.buttons.empty () || !input.buttons.front ().hasPosition)
        io.AddMousePosEvent (input.pointer ? input.x : -FLT_MAX, input.pointer ? input.y : -FLT_MAX);
    for (const Input::Button& button : input.buttons) {
        if (button.hasPosition)
            io.AddMousePosEvent (button.x, button.y);
        io.AddMouseButtonEvent (button.button, button.down);
    }
    io.AddMousePosEvent (input.pointer ? input.x : -FLT_MAX, input.pointer ? input.y : -FLT_MAX);
    if (input.hasButtonState)
        for (int button = 0; button < 5; ++button)
            if ((input.buttonsDown & (1u << button)) == 0 &&
                (io.MouseDown[button] ||
                 std::any_of (input.buttons.begin (), input.buttons.end (),
                              [button] (const Input::Button& event) { return event.button == button && event.down; })))
                io.AddMouseButtonEvent (button, false);
}

void Engine::Impl::Ramp (const layers::Panel& panel, const layers::PanelItem& item, size_t index, PanelState& state,
                         float width, float scale)
{
    double band[2] = {};
    const std::string& control = item.colormap.cutoffControl;
    const std::string& greenControl = item.colormap.greenControl;
    if (control.empty () && greenControl.empty ()) {
        if (items::Ramp (panel, item, width, scale, band))
            highlight = { true, layer, band[0], band[1] };
        return;
    }
    double* cutoff = control.empty () ? nullptr : &state.values[control].current;
    double* green = greenControl.empty () ? nullptr : &state.values[greenControl].current;
    const double previousCutoff = cutoff != nullptr ? *cutoff : 0;
    const double previousGreen = green != nullptr ? *green : 0;
    bool cutoffReleased = false, greenReleased = false;
    items::Ramp (panel, item, width, scale, band, cutoff, &cutoffReleased, green, &greenReleased);
    const auto queue = [&] (const char* kind, const std::string& id, double value, bool released,
                            std::vector<Change>& pending) {
        Change change { kind,
                        key,
                        panel.title,
                        id,
                        int32_t (index),
                        value,
                        items::Number (value, item.decimals) + " " + item.unit,
                        released };
        changes.push_back (change);
        pending.push_back (std::move (change));
    };
    if (cutoff != nullptr && (previousCutoff != *cutoff || cutoffReleased))
        queue ("rampCutoff", control, *cutoff, cutoffReleased, store->rampCutoffs);
    if (green != nullptr && (previousGreen != *green || greenReleased))
        queue ("rampGreen", greenControl, *green, greenReleased, store->rampGreens);
}

void Engine::Impl::SitePlan (const layers::Panel& panel, const layers::PanelItem& item, size_t index, PanelState& state,
                             float width, float scale)
{
    const size_t count = item.outlineXY.size () / 2;
    if (count < 3 || item.setbackDistances.size () != count || item.setbackModes.size () != count)
        return;
    const ImVec2 origin = ImGui::GetCursorScreenPos ();
    const ImVec2 extent ((std::max) (80.0f, width), (item.heightPixels > 0 ? item.heightPixels : 240) * scale);
    double minX = item.outlineXY[0], maxX = minX, minY = item.outlineXY[1], maxY = minY;
    for (size_t v = 0; v < count; ++v) {
        minX = (std::min) (minX, item.outlineXY[v * 2]);
        maxX = (std::max) (maxX, item.outlineXY[v * 2]);
        minY = (std::min) (minY, item.outlineXY[v * 2 + 1]);
        maxY = (std::max) (maxY, item.outlineXY[v * 2 + 1]);
    }
    const float padding = 20 * scale;
    const double factor = (std::min) ((extent.x - 2 * padding) / (std::max) (maxX - minX, 1e-9),
                                      (extent.y - 2 * padding) / (std::max) (maxY - minY, 1e-9));
    auto project = [&] (double x, double y) {
        return ImVec2 (origin.x + extent.x * 0.5f + float ((x - (minX + maxX) * 0.5) * factor),
                       origin.y + extent.y * 0.5f - float ((y - (minY + maxY) * 0.5) * factor));
    };
    std::vector<ImVec2> boundary, offsetRegion;
    for (size_t v = 0; v < count; ++v)
        boundary.push_back (project (item.outlineXY[v * 2], item.outlineXY[v * 2 + 1]));
    for (size_t v = 0; v < item.offsetXY.size () / 2; ++v)
        offsetRegion.push_back (project (item.offsetXY[v * 2], item.offsetXY[v * 2 + 1]));
    ImGui::InvisibleButton ("##sitePlan", extent, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered ();
    ImDrawList* draw = ImGui::GetWindowDrawList ();
    draw->AddRectFilled (origin, ImVec2 (origin.x + extent.x, origin.y + extent.y), IM_COL32 (245, 247, 249, 255),
                         4 * scale);
    draw->PushClipRect (origin, ImVec2 (origin.x + extent.x, origin.y + extent.y), true);
    if (offsetRegion.size () >= 3) {
        // The caller sends the compact solver's convex inset, not an arbitrary polygon fill.
        draw->AddConvexPolyFilled (offsetRegion.data (), int (offsetRegion.size ()), IM_COL32 (220, 243, 250, 180));
        draw->AddPolyline (offsetRegion.data (), int (offsetRegion.size ()), IM_COL32 (166, 98, 38, 255),
                           ImDrawFlags_Closed, 1.5f * scale);
    }
    const int selected = int (state.values[item.id + ":selected"].current);
    const ImVec2 pointer = ImGui::GetIO ().MousePos;
    int vertexHit = -1, edgeHit = -1;
    float vertexDistance = 9 * scale, edgeDistance = 10 * scale;
    for (size_t v = 0; v < count; ++v) {
        const ImVec2 a = boundary[v], b = boundary[(v + 1) % count];
        draw->AddLine (a, b, IM_COL32 (170, 68, 101, 255),
                       int (v) == selected && item.editable ? 3 * scale : 1.5f * scale);
        const float dx = pointer.x - a.x, dy = pointer.y - a.y;
        const float d = std::sqrt (dx * dx + dy * dy);
        if (d < vertexDistance) {
            vertexDistance = d;
            vertexHit = int (v);
        }
        const float ex = b.x - a.x, ey = b.y - a.y;
        const float length = ex * ex + ey * ey;
        const float t = length > 0 ? (std::clamp) ((dx * ex + dy * ey) / length, 0.0f, 1.0f) : 0;
        const float px = dx - t * ex, py = dy - t * ey;
        const float distance = std::sqrt (px * px + py * py);
        if (distance < edgeDistance) {
            edgeDistance = distance;
            edgeHit = int (v);
        }
        const bool used = state.values[item.id + ":point:" + std::to_string (v)].current != 0;
        draw->AddCircleFilled (a, 4 * scale, used ? IM_COL32 (47, 111, 235, 255) : IM_COL32 (170, 180, 185, 255));
        const std::string text = "S" + std::to_string (v + 1);
        draw->AddText (ImVec2 ((a.x + b.x) * 0.5f + 3 * scale, (a.y + b.y) * 0.5f), IM_COL32 (100, 75, 85, 255),
                       text.c_str ());
    }
    draw->PopClipRect ();
    auto said = [&] (const char* kind, const std::string& id, double value) {
        changes.push_back ({ kind, key, panel.title, id, int32_t (index), value, "", true });
    };
    if (hovered && item.editable && (vertexHit >= 0 || edgeHit >= 0)) {
        hand = true;
        if (ImGui::IsMouseReleased (ImGuiMouseButton_Right)) {
            hudshell::ClaimRightClick ();
            state.values[item.id + ":targetPoint"].current = vertexHit;
            state.values[item.id + ":targetEdge"].current = edgeHit;
            if (edgeHit >= 0 && vertexHit < 0) {
                state.values[item.id + ":selected"].current = edgeHit;
                said ("combo", item.id + ":selected", edgeHit);
            }
            ImGui::OpenPopup ("##site.context");
        }
    }
    if (!item.editable)
        return;
    if (ImGui::BeginPopup ("##site.context")) {
        const int vertex = int (state.values[item.id + ":targetPoint"].current);
        const int edge = int (state.values[item.id + ":targetEdge"].current);
        if (vertex >= 0 && size_t (vertex) < count) {
            const std::string id = item.id + ":point:" + std::to_string (vertex);
            bool used = state.values[id].current != 0;
            ImGui::Text ("Endpoint %d", vertex + 1);
            if (ImGui::Checkbox ("Use for average elevation", &used)) {
                state.values[id].current = used ? 1 : 0;
                said ("checkbox", id, used ? 1 : 0);
            }
        }
        else if (edge >= 0 && size_t (edge) < count) {
            const std::string modeId = item.id + ":mode:" + std::to_string (edge);
            const std::string distanceId = item.id + ":offset:" + std::to_string (edge);
            ImGui::Text ("Segment %d", edge + 1);
            int mode = int (state.values[modeId].current);
            const std::string defaultLabel = "Default (" + items::Number (item.number, 2) + " m)";
            const char* names[] = { defaultLabel.c_str (), "Custom", "None (0 m)" };
            for (int m = 0; m < 3; ++m) {
                if (ImGui::RadioButton (names[m], mode == m)) {
                    mode = m;
                    state.values[modeId].current = m;
                    said ("combo", modeId, m);
                }
            }
            if (mode == 1) {
                double distance = state.values[distanceId].current;
                const double minDistance = 0, maxDistance = 1000;
                ImGui::SetNextItemWidth (140 * scale);
                // The overlay currently forwards mouse input, not keyboard text.
                // A numeric drag is usable in that backend; InputDouble is not.
                if (ImGui::DragScalar ("Offset", ImGuiDataType_Double, &distance, 0.01f, &minDistance, &maxDistance,
                                       "%.2f m", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp)) {
                    distance = std::round (distance * 100) / 100;
                    state.values[distanceId].current = distance;
                    said ("slider", distanceId, distance);
                }
                ImGui::TextUnformatted ("Drag to adjust (0.01 m steps)");
            }
        }
        ImGui::EndPopup ();
    }
}

bool Engine::Impl::TabItem (const layers::PanelItem& tab, uint32_t number, const std::string& bar, PanelState& state)
{
    const int held = int (state.values[bar].current);
    const auto shown = shownTabs.find (key + "/" + bar);
    const int before = shown == shownTabs.end () ? -1 : shown->second;
    // ⚠️ THE HELD TAB IS ASKED FOR ONLY WHERE THIS CONTEXT SHOWED ANOTHER -- the first
    // frame, Python's new choice, the other view's press. Asked every frame, it would be
    // applied over the user's click, which ImGui takes a frame later.
    const ImGuiTabItemFlags flags = held == int (number) && before != held ? ImGuiTabItemFlags_SetSelected : 0;
    const std::string label = tab.text + "###tab" + std::to_string (number);
    if (!ImGui::BeginTabItem (label.c_str (), nullptr, flags))
        return false;
    tabNow = int (number);
    return true;
}

void Engine::Impl::TabsDone (const layers::Panel& panel, size_t first, const std::string& bar, PanelState& state)
{
    int& before = shownTabs.try_emplace (key + "/" + bar, -1).first->second;
    State::Held& held = state.values[bar];
    // The user's press: a tab this context did not show last frame, and not the held one
    // it was asked to show.
    if (tabNow >= 0 && before >= 0 && tabNow != before && tabNow != int (held.current)) {
        held.current = double (tabNow);
        std::string title;
        int number = 0;
        for (size_t i = first; i < panel.items.size (); ++i)
            if (panel.items[i].kind == layers::ItemKind::Tab && number++ == tabNow)
                title = panel.items[i].text;
        changes.push_back ({ "tab", key, panel.title, bar, int32_t (first), held.current, title, true });
    }
    before = tabNow;
}

} // namespace overlayhud
} // namespace archviz
} // namespace geomsrv
