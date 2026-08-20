#include "APIEnvir.h"
#include "ACAPinc.h"

#include "AboutDialog.hpp"
#include "AddOnVersion.hpp"
#include "ResourceIds.hpp"
#include "Python/PathUtils.hpp"  // EvpDataDir, CreateDirectoryChain
#include "Python/PythonHost.hpp" // GetScriptsRoot

#include <shellapi.h>

namespace {

// Placeholder until Tapioca has a published release endpoint.
constexpr const wchar_t* DummyUpdateUrl = L"https://example.com/tapioca/updates";

} // namespace

AboutDialog::AboutDialog ()
    : DG::ModalDialog (ACAPI_GetOwnResModule (), AboutDialogResId, ACAPI_GetOwnResModule ()),
      okButton (GetReference (), AboutOkButtonId), logsButton (GetReference (), AboutLogsButtonId),
      commandsButton (GetReference (), AboutCommandsButtonId), updatesButton (GetReference (), AboutUpdatesButtonId),
      versionText (GetReference (), AboutVersionTextId)
{
    AttachToAllItems (*this);
    Attach (*this);

    // The .grc holds the format so the identity line stays localizable.
    versionText.SetText (GS::UniString::SPrintf (versionText.GetText (), GS::UniString (ADDON_VERSION).ToPrintf ()));
}

AboutDialog::~AboutDialog () = default;

void AboutDialog::ButtonClicked (const DG::ButtonClickEvent& ev)
{
    if (ev.GetSource () == &okButton) {
        PostCloseRequest (DG::ModalDialog::Accept);
    }
    else if (ev.GetSource () == &logsButton) {
        const GS::UniString dataDir (evp::EvpDataDir ());
        if (!dataDir.IsEmpty ())
            RevealFolder (GS::UniString (dataDir + GS::UniString ("\\logs")));
    }
    else if (ev.GetSource () == &commandsButton) {
        const GS::UniString root (evp::GetScriptsRoot ());
        if (!root.IsEmpty ())
            RevealFolder (root);
    }
    else if (ev.GetSource () == &updatesButton) {
        ShellExecuteW (nullptr, L"open", DummyUpdateUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void AboutDialog::RevealFolder (const GS::UniString& path)
{
    // Create it first: on a fresh install neither folder exists until something
    // writes, and a button that silently does nothing is worse than no button.
    evp::CreateDirectoryChain (path);
    // ToUStr() hands back a const GS::uchar_t* which IS a const wchar_t* under the
    // SDK's /Zc:wchar_t- — the idiom used throughout Python/PathUtils.cpp.
    ShellExecuteW (nullptr, L"open", (LPCWSTR) path.ToUStr ().Get (), nullptr, nullptr, SW_SHOWNORMAL);
}
