// ArchViz/Breadcrumb: the experiment guard's crash-loop file, held by the process that armed.
// Windows decides sharing per handle, so two holds taken here stand for two Archicads, and a
// handle closed without `Release` stands for a session that died armed.

#include "ArchViz/Breadcrumb.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace breadcrumb = geomsrv::archviz::breadcrumb;

namespace {

class BreadcrumbTest : public ::testing::Test {
  protected:
    void SetUp () override
    {
        folder_ = std::filesystem::temp_directory_path () /
                  (L"tapioca-breadcrumb-" + std::to_wstring (::GetCurrentProcessId ()) + L"-" +
                   std::to_wstring (reinterpret_cast<uintptr_t> (this)));
        std::filesystem::remove_all (folder_);
        std::filesystem::create_directories (folder_);
        base_ = (folder_ / L"EXPERIMENT_ARMED").wstring ();
        held_.reserve (16); // `Hold` hands out references into it
    }
    void TearDown () override
    {
        for (breadcrumb::Held& held : held_)
            breadcrumb::Release (held);
        std::error_code ignored;
        std::filesystem::remove_all (folder_, ignored);
    }
    breadcrumb::Held& Hold (const char* mode)
    {
        held_.push_back (breadcrumb::Hold (base_, mode));
        return held_.back ();
    }
    // What a process that died armed leaves: the file, and nobody holding it.
    void Die (breadcrumb::Held& held)
    {
        ::CloseHandle (HANDLE (held.file));
        held.file = nullptr;
    }
    // Read past the holder: it shares reads, and holds the file for writing and deleting.
    static std::string Contents (const std::wstring& path)
    {
        HANDLE file =
            ::CreateFileW (path.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return "(cannot open)";
        char buffer[256] = {};
        DWORD read = 0;
        ::ReadFile (file, buffer, sizeof (buffer), &read, nullptr);
        ::CloseHandle (file);
        return std::string (buffer, read);
    }
    static bool Exists (const std::wstring& path)
    {
        return ::GetFileAttributesW (path.c_str ()) != INVALID_FILE_ATTRIBUTES;
    }
    std::wstring Second () const
    {
        return base_ + L"-2";
    }
    std::filesystem::path folder_;
    std::wstring base_;
    std::vector<breadcrumb::Held> held_;
};

} // namespace

TEST (Breadcrumb, EachProcesssFileIsTheBaseThenNumbered)
{
    const std::vector<std::wstring> paths = breadcrumb::PathsOf (L"C:\\Tapioca\\EXPERIMENT_ARMED");
    ASSERT_EQ (paths.size (), size_t (breadcrumb::kMaxHolders));
    EXPECT_EQ (paths[0], L"C:\\Tapioca\\EXPERIMENT_ARMED");
    EXPECT_EQ (paths[1], L"C:\\Tapioca\\EXPERIMENT_ARMED-2");
    EXPECT_EQ (paths.back (), L"C:\\Tapioca\\EXPERIMENT_ARMED-9");
}

TEST_F (BreadcrumbTest, AProcessAloneHoldsTheBaseNamingItsMode)
{
    const breadcrumb::Held& held = Hold ("hookdiag");
    ASSERT_NE (held.file, nullptr);
    EXPECT_EQ (held.path, base_);
    EXPECT_EQ (Contents (base_), "hookdiag");
}

// The 2026-10-02 run: a second Archicad started while the first had the 3D overlay on.
TEST_F (BreadcrumbTest, ABreadcrumbARunningProcessHoldsIsNotACrash)
{
    Hold ("hookdiag");
    const breadcrumb::Found found = breadcrumb::Sweep (breadcrumb::PathsOf (base_));
    EXPECT_TRUE (found.left.empty ()) << "the second Archicad read the first's live breadcrumb as a crash";
    EXPECT_EQ (found.held, 1u);
    EXPECT_TRUE (Exists (base_)) << "the second Archicad deleted the first's breadcrumb";
}

TEST_F (BreadcrumbTest, ASecondProcessArmedHoldsAFileOfItsOwn)
{
    Hold ("hookdiag");
    const breadcrumb::Held& second = Hold ("planoverlay");
    ASSERT_NE (second.file, nullptr) << "the second Archicad could not arm";
    EXPECT_EQ (second.path, Second ());
    EXPECT_EQ (Contents (base_), "hookdiag");
    EXPECT_EQ (Contents (Second ()), "planoverlay");
}

TEST_F (BreadcrumbTest, OneProcessDisarmingLeavesTheOthersBreadcrumb)
{
    breadcrumb::Held& first = Hold ("hookdiag");
    Hold ("hookdiag");
    breadcrumb::Release (first);
    EXPECT_FALSE (Exists (base_));
    EXPECT_TRUE (Exists (Second ()));
    const breadcrumb::Found found = breadcrumb::Sweep (breadcrumb::PathsOf (base_));
    EXPECT_TRUE (found.left.empty ());
    EXPECT_EQ (found.held, 1u);
}

TEST_F (BreadcrumbTest, ABreadcrumbLeftByASessionThatDiedIsACrashAndGoes)
{
    Hold ("hookdiag");
    Die (Hold ("planoverlay"));
    const breadcrumb::Found found = breadcrumb::Sweep (breadcrumb::PathsOf (base_));
    ASSERT_EQ (found.left.size (), 1u);
    EXPECT_EQ (found.left.front (), "planoverlay");
    EXPECT_EQ (found.held, 1u);
    EXPECT_FALSE (Exists (Second ())) << "one bad launch must cost one degraded session, not a loop";
    EXPECT_TRUE (Exists (base_));
}

TEST_F (BreadcrumbTest, ACleanDisarmLeavesNothingBehind)
{
    breadcrumb::Release (Hold ("hookdiag"));
    EXPECT_FALSE (Exists (base_));
    const breadcrumb::Found found = breadcrumb::Sweep (breadcrumb::PathsOf (base_));
    EXPECT_TRUE (found.left.empty ());
    EXPECT_EQ (found.held, 0u);
}

TEST_F (BreadcrumbTest, ArmedAgainTheBreadcrumbNamesOnlyTheNewMode)
{
    breadcrumb::Held& held = Hold ("planoverlay");
    ASSERT_TRUE (breadcrumb::Rewrite (held, "hookdiag"));
    EXPECT_EQ (Contents (base_), "hookdiag");
    Die (held);
    const breadcrumb::Found found = breadcrumb::Sweep (breadcrumb::PathsOf (base_));
    ASSERT_EQ (found.left.size (), 1u);
    EXPECT_EQ (found.left.front (), "hookdiag");
}

TEST_F (BreadcrumbTest, ABreadcrumbWrittenAndClosedAsTheEarlierBuildDidIsACrash)
{
    const std::wstring legacy = (folder_ / L"ARMED_hookdiag").wstring ();
    HANDLE file =
        ::CreateFileW (legacy.c_str (), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE (file, INVALID_HANDLE_VALUE);
    ::CloseHandle (file);
    const breadcrumb::Found found = breadcrumb::Sweep ({ base_, legacy });
    ASSERT_EQ (found.left.size (), 1u);
    EXPECT_EQ (found.left.front (), "") << "an empty breadcrumb still blocks; the guard names it hand-written";
    EXPECT_FALSE (Exists (legacy));
}
