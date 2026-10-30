#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditEllipse.hpp"
#include "Edit/EditOrganize.hpp"
#include <algorithm>

namespace geomsrv::archviz::edithud::detail {
namespace {
constexpr double kDegrees = 180.0 / 3.14159265358979323846;

bool EllipseLocked (const Session& s, edit::Id id)
{
    return GizmoModeLocked (s) || s.edits.InGesture () || !s.drawing.empty () || s.penDown || s.terrainCapture ||
           s.draftingImport || s.debugExportRequested || !edit::Pickable (Doc (s), id);
}

bool ApplyEllipse (Session& s, const edit::Operation& op)
{
    const edit::Entity* entity = SelectedEllipse (s);
    if (entity == nullptr || EllipseLocked (s, entity->id)) {
        s.message = "Select one unlocked circle or ellipse and finish the current edit first.";
        return false;
    }
    if (!s.edits.Apply (op)) {
        s.message = s.edits.Message ();
        return false;
    }
    s.message.clear ();
    return true;
}
} // namespace

const edit::Entity* SelectedEllipse (const Session& s)
{
    const auto& items = s.selection.Items ();
    if (items.empty ())
        return nullptr;
    const edit::Id id = items.front ().entity;
    if (std::any_of (items.begin (), items.end (), [id] (const edit::Ref& r) { return r.entity != id; }))
        return nullptr;
    const edit::Entity* entity = edit::Find (Doc (s), id);
    return entity != nullptr && entity->ellipse ? entity : nullptr;
}

bool SetSelectedEllipseValue (Session& s, EllipseField field, double value)
{
    const edit::Entity* entity = SelectedEllipse (s);
    if (entity == nullptr)
        return false;
    const edit::Id id = entity->id;
    return ApplyEllipse (s, [id, field, value] (edit::Document& doc, std::string& error) {
        edit::EllipseParameters parameters;
        const edit::Entity* current = edit::Find (doc, id);
        if (current == nullptr || !edit::GetEllipseParameters (*current, parameters, error))
            return false;
        switch (field) {
            case EllipseField::CenterX:
            case EllipseField::CenterY:
            case EllipseField::CenterZ: {
                edit::Vec3 center = parameters.center;
                if (field == EllipseField::CenterX)
                    center.x = value;
                else if (field == EllipseField::CenterY)
                    center.y = value;
                else
                    center.z = value;
                return edit::SetEllipseCenter (doc, id, center, error);
            }
            case EllipseField::MajorRadius:
                return edit::SetEllipseRadii (
                    doc, id, value, parameters.mode == edit::EllipseMode::Circle ? value : parameters.minorRadius,
                    error);
            case EllipseField::MinorRadius:
                return edit::SetEllipseRadii (doc, id, parameters.majorRadius, value, error);
            case EllipseField::Rotation:
                return edit::SetEllipseRotation (doc, id, value / kDegrees, error);
        }
        error = "unknown ellipse field";
        return false;
    });
}

bool SetSelectedEllipseMode (Session& s, edit::EllipseMode mode)
{
    const edit::Entity* entity = SelectedEllipse (s);
    if (entity == nullptr)
        return false;
    const edit::Id id = entity->id;
    return ApplyEllipse (s, [id, mode] (edit::Document& doc, std::string& error) {
        edit::EllipseParameters parameters;
        const edit::Entity* current = edit::Find (doc, id);
        if (current == nullptr || !edit::GetEllipseParameters (*current, parameters, error))
            return false;
        parameters.mode = mode;
        if (mode == edit::EllipseMode::Circle)
            parameters.minorRadius = parameters.majorRadius;
        return edit::SetEllipseParameters (doc, id, parameters, error);
    });
}

bool ConvertSelectedEllipse (Session& s)
{
    const edit::Entity* entity = SelectedEllipse (s);
    if (entity == nullptr)
        return false;
    const edit::Id id = entity->id;
    return ApplyEllipse (
        s, [id] (edit::Document& doc, std::string& error) { return edit::ConvertEllipseToFreeform (doc, id, error); });
}

void EllipseControls (Session& s)
{
    const edit::Entity* entity = SelectedEllipse (s);
    if (entity == nullptr)
        return;
    edit::EllipseParameters parameters;
    std::string error;
    if (!edit::GetEllipseParameters (*entity, parameters, error)) {
        ImGui::TextWrapped ("%s", error.c_str ());
        return;
    }
    const bool circle = parameters.mode == edit::EllipseMode::Circle;
    const edit::Id id = entity->id;
    ImGui::PushID (std::to_string (id).c_str ());
    ImGui::Separator ();
    ImGui::TextUnformatted (circle ? "Circle parameters" : "Ellipse parameters");
    ImGui::TextWrapped ("Metres; rotation in degrees in the ellipse plane. Enter applies.");
    ImGui::BeginDisabled (EllipseLocked (s, id));
    // Enter commits once through Session. Keep doubles throughout; display rounding must
    // never become an unedited parameter's value, or a float-limited tracker increment.
    const auto input = [&] (const char* label, EllipseField field, double value, bool disabled = false) {
        ImGui::BeginDisabled (disabled);
        ImGui::SetNextItemWidth (180.0f);
        if (ImGui::InputDouble (label, &value, 0, 0, "%.17g", ImGuiInputTextFlags_EnterReturnsTrue))
            SetSelectedEllipseValue (s, field, value);
        ImGui::EndDisabled ();
    };
    input ("Centre X", EllipseField::CenterX, parameters.center.x);
    input ("Centre Y", EllipseField::CenterY, parameters.center.y);
    input ("Centre Z", EllipseField::CenterZ, parameters.center.z);
    input ("Major radius", EllipseField::MajorRadius, parameters.majorRadius);
    input ("Minor radius", EllipseField::MinorRadius, parameters.minorRadius, circle);
    input ("Rotation", EllipseField::Rotation, parameters.rotation * kDegrees);
    if (ImGui::RadioButton ("Circle (use major radius)", circle))
        SetSelectedEllipseMode (s, edit::EllipseMode::Circle);
    ImGui::SameLine ();
    if (ImGui::RadioButton ("Ellipse", !circle))
        SetSelectedEllipseMode (s, edit::EllipseMode::Ellipse);
    ImGui::TextWrapped ("Anchors and handles are derived, not freeform. Convert to curve to edit them. "
                        "Warning: conversion loses circle/ellipse parameters and keeps the cubic approximation. "
                        "Undo restores the definition.");
    if (ImGui::Button ("Convert to curve"))
        ConvertSelectedEllipse (s);
    ImGui::EndDisabled ();
    ImGui::PopID ();
}
} // namespace geomsrv::archviz::edithud::detail
