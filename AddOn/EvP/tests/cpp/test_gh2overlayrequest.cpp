#include "Grasshopper/Gh2OverlayRequest.hpp"
#include "Preview/PublicationAdmission.hpp"
#include <gtest/gtest.h>

using namespace evp::grasshopper;

TEST (PublicationAdmission, ReplacementBetweenPreflightAndAdoptionRejectsOldWork)
{
    tapioca::preview::PublicationAdmission state;
    tapioca::preview::PublicationAdmission::Token queued { state.Epoch (), "producer", std::string (32, 'a'), 1 };
    ASSERT_TRUE (state.Current (queued));
    state.ProjectChanged ();
    EXPECT_FALSE (state.Adopt (queued));
    queued.epoch = state.Epoch ();
    EXPECT_TRUE (state.Adopt (queued));
}

TEST (PublicationAdmission, ClearTombstoneRejectsLateResultsAndOtherDocuments)
{
    tapioca::preview::PublicationAdmission state;
    tapioca::preview::PublicationAdmission::Token show { state.Epoch (), "producer", std::string (32, 'a'), 1 };
    auto clear = show;
    clear.revision = 3;
    EXPECT_TRUE (state.Adopt (clear));
    EXPECT_FALSE (state.Adopt (show));
    EXPECT_FALSE (state.Adopt (clear));
    show.revision = 4;
    show.document = std::string (32, 'b');
    EXPECT_FALSE (state.Adopt (show));
    show.document = clear.document;
    EXPECT_TRUE (state.Adopt (show));
    show.producer = "other";
    show.revision = 1;
    EXPECT_TRUE (state.Adopt (show));
}

TEST (PublicationAdmission, DisconnectSealsGenerationWhileReplacementCanPublish)
{
    tapioca::preview::PublicationAdmission state;
    tapioca::preview::PublicationAdmission::Token old { state.Epoch (), "session.1.producer", std::string (32, 'a'),
                                                        1 };
    ASSERT_TRUE (state.Current (old));
    state.ReleasePrefix ("session.1.");
    EXPECT_FALSE (state.Adopt (old));
    old.producer = "session.2.producer";
    EXPECT_TRUE (state.Adopt (old));
}

#ifdef _WIN32
#include "Grasshopper/PipeTransfer.hpp"
#include <future>
#include <thread>

TEST (PipeTransfer, ReadyReplyWakesIdlePartialReadWithoutHeartbeat)
{
    const std::wstring name = L"\\\\.\\pipe\\TapiocaTransferTest." + std::to_wstring (GetCurrentProcessId ());
    HANDLE server = CreateNamedPipeW (name.c_str (), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                      PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    ASSERT_NE (server, INVALID_HANDLE_VALUE);
    HANDLE client = CreateFileW (name.c_str (), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE (client, INVALID_HANDLE_VALUE);
    HANDLE wake = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> stopping { false };
    std::promise<void> flushed;
    auto ready = flushed.get_future ();
    char buffer[2] {};
    bool read = false;
    bool wrote = false;
    std::thread io ([&] {
        read = tapioca::transport::TransferExact (server, buffer, 2, false, stopping, 0, wake,
                                                 [&] {
                                                     char response = 'r';
                                                     wrote = tapioca::transport::TransferExact (server, &response, 1,
                                                                                                true, stopping, 1000);
                                                     flushed.set_value ();
                                                 });
    });
    DWORD count = 0;
    EXPECT_TRUE (WriteFile (client, "a", 1, &count, nullptr));
    SetEvent (wake);
    EXPECT_EQ (ready.wait_for (std::chrono::seconds (2)), std::future_status::ready);
    EXPECT_TRUE (WriteFile (client, "b", 1, &count, nullptr));
    io.join ();
    EXPECT_TRUE (read);
    EXPECT_TRUE (wrote);
    EXPECT_EQ (std::string (buffer, 2), "ab");
    CloseHandle (wake);
    CloseHandle (client);
    CloseHandle (server);
}
#endif

TEST (Gh2OverlayRequest, OwnGenerationOnly)
{
    const std::string layer = "gh2.12.0123456789abcdef0123456789abcdef";
    EXPECT_TRUE (IsGh2OverlayLayer (layer, 12));
    EXPECT_FALSE (IsGh2OverlayLayer (layer, 13));
    EXPECT_FALSE (IsGh2OverlayLayer (layer + "0", 12));
    EXPECT_FALSE (IsGh2OverlayLayer ("gh2.12.not-a-guid", 12));
    EXPECT_FALSE (IsGh2OverlayLayer ("gh2.0.0123456789abcdef0123456789abcdef", 0));
}

TEST (Gh2OverlayRequest, SeparateViewsAndNoGlobalClearOrOff)
{
    const std::string start = "{\"layer\":\"gh2.12.0123456789abcdef0123456789abcdef\"";
    EXPECT_TRUE (IsGh2OverlayRequest ("Tapioca.SetOverlayLayer", start + ",\"views\":\"2d\",\"polylines\":[]}", 12));
    EXPECT_TRUE (IsGh2OverlayRequest ("Tapioca.SetOverlayLayer", start + ",\"views\":\"3d\",\"meshes\":[]}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.SetOverlayLayer", start + ",\"views\":\"both\",\"meshes\":[]}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.SetOverlayLayer", start + ",\"views\":\"3d\",\"meshes\":[]}", 13));
    EXPECT_TRUE (IsGh2OverlayRequest (
        "Tapioca.ClearOverlayLayer",
        start + ",\"projectEpoch\":1,\"documentSession\":\"0123456789abcdef0123456789abcdef\",\"revision\":2}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.ClearOverlayLayer", start + "}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.ClearOverlayLayer", start + ",\"all\":true}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.ClearOverlayLayer", "{\"all\":true}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.Overlay2D", "{\"action\":\"on\"}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.Overlay3D", "{\"action\":\"on\"}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.Overlay3D", "{\"action\":\"off\"}", 12));
    EXPECT_FALSE (IsGh2OverlayRequest ("Tapioca.DeleteElements", start + "}", 12));
    EXPECT_FALSE (
        IsGh2OverlayRequest ("Tapioca.SetOverlayLayer",
                             start + ",\"views\":\"3d\",\"notes\":\"" + std::string (1024 * 1024, 'x') + "\"}", 12));
}
