#ifndef EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_SCOPEDSTYLE_HPP
#define EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_SCOPEDSTYLE_HPP

// TapiocaUI/Foundation/ScopedStyle -- ImGui's style pushes, popped when the scope ends.
//
// ⚠️ A MISSED POP LEAKS ONE FEATURE'S LOOK INTO THE NEXT. hudshell::PushLook returns a colour
// count the caller must balance by hand, and every early return between the push and the pop
// is a place to forget it. Pushing through this object makes the balance the destructor's job:
// colours, style variables and fonts are popped in reverse, whatever path leaves the scope.
//
// `Adopt` takes over pushes a legacy helper already made (PushLook's colours and its
// `kLookVars`), so a caller can stop counting before that helper itself is rewritten.
//
// Pure ImGui on the thread that holds the context; one object per scope, never shared.

#include <imgui.h>

namespace geomsrv {
namespace archviz {
namespace ui {

class ScopedStyle {
  public:
    ScopedStyle () = default;
    ~ScopedStyle ();
    ScopedStyle (const ScopedStyle&) = delete;
    ScopedStyle& operator= (const ScopedStyle&) = delete;

    ScopedStyle& Colour (ImGuiCol index, ImU32 colour);
    ScopedStyle& Colour (ImGuiCol index, const ImVec4& colour);
    ScopedStyle& Var (ImGuiStyleVar index, float value);
    ScopedStyle& Var (ImGuiStyleVar index, const ImVec2& value);
    // `font` null keeps the current font; `size` 0 keeps the current size (ImGui 1.92 PushFont).
    ScopedStyle& Font (ImFont* font, float size);

    // Pushes already made by someone else, now this scope's to pop.
    ScopedStyle& Adopt (int colours, int vars);

    // Pops everything now, before the scope ends; the destructor then has nothing left to do.
    void Pop ();

    int Colours () const
    {
        return colours_;
    }
    int Vars () const
    {
        return vars_;
    }
    int Fonts () const
    {
        return fonts_;
    }

  private:
    int colours_ = 0;
    int vars_ = 0;
    int fonts_ = 0;
};

} // namespace ui
} // namespace archviz
} // namespace geomsrv

#endif
