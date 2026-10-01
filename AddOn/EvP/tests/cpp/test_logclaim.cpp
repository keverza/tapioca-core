// ArchViz/LogClaim: which file a process's log writes when another process writes archviz.log.
// Windows decides sharing per handle, not per process, so two claims taken here stand for two
// Archicads; a handle opened as the earlier build opened it stands for an Archicad running that.

#include "ArchViz/LogClaim.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace logclaim = geomsrv::archviz::logclaim;

namespace {

// A folder of the test's own, emptied before and removed after.
class LogClaimTest : public ::testing::Test {
  protected:
    void SetUp () override
    {
        folder_ = std::filesystem::temp_directory_path () /
                  (L"tapioca-logclaim-" + std::to_wstring (::GetCurrentProcessId ()) + L"-" +
                   std::to_wstring (reinterpret_cast<uintptr_t> (this)));
        std::filesystem::remove_all (folder_);
        std::filesystem::create_directories (folder_);
        base_ = (folder_ / L"archviz.log").wstring ();
    }
    void TearDown () override
    {
        for (void* file : held_)
            ::CloseHandle (HANDLE (file));
        std::error_code ignored;
        std::filesystem::remove_all (folder_, ignored);
    }
    logclaim::Claim Take ()
    {
        logclaim::Claim claim = logclaim::Take (base_, "12:00:00");
        if (claim.file != nullptr)
            held_.push_back (claim.file);
        return claim;
    }
    std::string Read (const std::wstring& path) const
    {
        std::ifstream in (path, std::ios::binary);
        return std::string (std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char> ());
    }
    std::wstring At (const wchar_t* name) const
    {
        return (folder_ / name).wstring ();
    }
    std::filesystem::path folder_;
    std::wstring base_;
    std::vector<void*> held_;
};

std::string Pid ()
{
    return "pid " + std::to_string (::GetCurrentProcessId ());
}

} // namespace

TEST (LogClaim, TheNthWritersFileIsNumberedBeforeItsExtension)
{
    EXPECT_EQ (logclaim::WriterPath (L"C:\\logs\\archviz.log", 1), L"C:\\logs\\archviz.log");
    EXPECT_EQ (logclaim::WriterPath (L"C:\\logs\\archviz.log", 2), L"C:\\logs\\archviz-2.log");
    EXPECT_EQ (logclaim::WriterPath (L"C:\\a.b\\log", 3), L"C:\\a.b\\log-3");
}

TEST (LogClaim, RotationKeepsTheSharedSchemeForEveryWritersFile)
{
    EXPECT_EQ (logclaim::BackupPath (L"C:\\logs\\archviz.log"), L"C:\\logs\\archviz.1.log");
    EXPECT_EQ (logclaim::BackupPath (L"C:\\logs\\archviz-2.log"), L"C:\\logs\\archviz-2.1.log");
    EXPECT_EQ (logclaim::BackupPath (L"C:\\a.b\\log"), L"C:\\a.b\\log.1");
}

TEST_F (LogClaimTest, AWriterAloneTakesArchvizLogAndSaysNothing)
{
    const logclaim::Claim claim = Take ();
    ASSERT_NE (claim.file, nullptr);
    EXPECT_EQ (claim.writer, 1u);
    EXPECT_EQ (claim.path, base_);
    EXPECT_EQ (claim.bytes, 0u);
    EXPECT_EQ (Read (base_), "");
}

TEST_F (LogClaimTest, ASecondWriterTakesAFileOfItsOwnAndSaysWhereInBoth)
{
    const logclaim::Claim first = Take ();
    const logclaim::Claim second = Take ();
    ASSERT_NE (second.file, nullptr) << "the second Archicad's lines went nowhere";
    EXPECT_EQ (second.writer, 2u);
    EXPECT_EQ (second.path, At (L"archviz-2.log"));

    const std::string pointer = Read (base_);
    EXPECT_NE (pointer.find ("writes its lines to archviz-2.log"), std::string::npos) << pointer;
    EXPECT_NE (pointer.find (Pid ()), std::string::npos) << pointer;
    EXPECT_EQ (pointer.rfind ("12:00:00  LOG          ", 0), 0u) << pointer;

    const std::string own = Read (second.path);
    EXPECT_EQ (own.rfind ("12:00:00  LOG          archviz.log is written by another process", 0), 0u) << own;
    EXPECT_EQ (second.bytes, own.size ());

    // The first keeps writing its file after the pointer line.
    DWORD written = 0;
    ASSERT_NE (::WriteFile (HANDLE (first.file), "after\r\n", 7, &written, nullptr), 0);
    EXPECT_EQ (Read (base_).substr (pointer.size ()), "after\r\n");
}

TEST_F (LogClaimTest, AThirdWriterPointsFromEveryFileItPassedOver)
{
    Take ();
    Take ();
    const logclaim::Claim third = Take ();
    ASSERT_NE (third.file, nullptr);
    EXPECT_EQ (third.writer, 3u);
    EXPECT_NE (Read (base_).find ("writes its lines to archviz-3.log"), std::string::npos);
    EXPECT_NE (Read (At (L"archviz-2.log")).find ("writes its lines to archviz-3.log"), std::string::npos);
    EXPECT_NE (Read (third.path).find ("archviz.log, archviz-2.log are written by another process"), std::string::npos)
        << Read (third.path);
}

TEST_F (LogClaimTest, AReaderTailingTheLogIsNotAWriter)
{
    HANDLE tail = ::CreateFileW (base_.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE (tail, INVALID_HANDLE_VALUE);
    const logclaim::Claim claim = Take ();
    EXPECT_EQ (claim.writer, 1u);
    ::CloseHandle (tail);

    // And the writer's file can still be tailed.
    HANDLE again = ::CreateFileW (base_.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE (again, INVALID_HANDLE_VALUE);
    if (again != INVALID_HANDLE_VALUE)
        ::CloseHandle (again);
}

TEST_F (LogClaimTest, AWriterGoneLetsGoOfItsFile)
{
    const logclaim::Claim first = Take ();
    ::CloseHandle (HANDLE (first.file));
    held_.clear ();
    const logclaim::Claim next = Take ();
    EXPECT_EQ (next.writer, 1u);
}

TEST_F (LogClaimTest, BesideAnArchicadOnTheEarlierBuildItStillTakesAFileAndSaysWhy)
{
    // The earlier build held archviz.log sharing only reads: no pointer line can go in.
    HANDLE earlier = ::CreateFileW (base_.c_str (), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE (earlier, INVALID_HANDLE_VALUE);
    const logclaim::Claim claim = Take ();
    ::CloseHandle (earlier);
    ASSERT_NE (claim.file, nullptr);
    EXPECT_EQ (claim.writer, 2u);
    EXPECT_NE (Read (claim.path).find ("archviz.log is written by another process"), std::string::npos);
    EXPECT_EQ (Read (base_), "");
}
