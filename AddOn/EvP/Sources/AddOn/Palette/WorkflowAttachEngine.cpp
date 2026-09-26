#include "WorkflowAttachEngine.hpp"

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"
#include "DGCommandDescriptor.hpp"
#include "DGMenu.hpp"
#include "DGMenuItem.hpp"
#include "ResourceMDIDIds.hpp"

namespace evp::palette {

namespace {

// Unique within our MDID; the palette context menu owns the lower ids.
constexpr ULong Gh1CommandId = 10001;
constexpr ULong Gh2CommandId = 10002;

class EngineMenu {
  public:
    EngineMenu () : gh1 (Gh1CommandId, AC_MDID_DEV, AC_MDID_LOC), gh2 (Gh2CommandId, AC_MDID_DEV, AC_MDID_LOC)
    {
        commands.Add (gh1, new DG::CommandDescriptor (gh1, "Rhino 8 / Grasshopper 1"));
        commands.Add (gh2, new DG::CommandDescriptor (gh2, "Rhino 9 / Grasshopper 2"));
    }

    ~EngineMenu ()
    {
        for (auto it = commands.EnumerateValues (); it != nullptr; ++it)
            delete *it;
    }

    std::optional<bool> Display (const DG::NativePoint& at)
    {
        DG::Menu menu ("-");
        menu.AddMenuItem (DG::MenuSimpleItem (gh1));
        menu.AddMenuItem (DG::MenuSimpleItem (gh2));
        DG::ContextMenu popup ("-", &menu);
        popup.SetEnabledCommands (commands);
        DG::CommandEvent* chosen = popup.Display (at);
        if (chosen == nullptr)
            return std::nullopt;
        const bool isGh2 = chosen->GetCommand ().GetCommandId () == Gh2CommandId;
        delete chosen;
        return isGh2;
    }

  private:
    DG::Command gh1;
    DG::Command gh2;
    DG::CommandTable commands;
};

} // namespace

std::optional<bool> ChooseWorkflowAttachEngine ()
{
    DG::MousePosData mouse;
    if (!mouse.Retrieve ())
        return std::nullopt;

    // A single Connect button serves both engines. The server must select its
    // versioned pipe before a Rhino client can discover it, not after attach.
    static EngineMenu menu;
    return menu.Display (mouse.GetMouseOffsetInNativeUnits ());
}

} // namespace evp::palette
