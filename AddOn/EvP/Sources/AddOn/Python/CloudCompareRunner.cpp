#include "APIEnvir.h" // Win32 only; this boundary must not include ACAPinc.h

#include "CloudCompareCommand.hpp"
#include "CloudCompareRunner.hpp"
#include "RunCancel.hpp"

#include <string>
#include <vector>

namespace evp {

namespace {

constexpr DWORD CloudCompareCancelExitCode = 0xE9;

struct Pipe {
    HANDLE read = nullptr;
    HANDLE write = nullptr;

    bool Create ()
    {
        SECURITY_ATTRIBUTES attributes = {};
        attributes.nLength = sizeof (attributes);
        attributes.bInheritHandle = TRUE;
        if (!CreatePipe (&read, &write, &attributes, 0))
            return false;
        if (SetHandleInformation (read, HANDLE_FLAG_INHERIT, 0) == 0) {
            Close ();
            return false;
        }
        return true;
    }

    void CloseRead ()
    {
        if (read != nullptr) {
            CloseHandle (read);
            read = nullptr;
        }
    }

    void CloseWrite ()
    {
        if (write != nullptr) {
            CloseHandle (write);
            write = nullptr;
        }
    }

    void Close ()
    {
        CloseRead ();
        CloseWrite ();
    }

    ~Pipe ()
    {
        Close ();
    }
};

struct Job {
    HANDLE handle = nullptr;

    ~Job ()
    {
        if (handle != nullptr)
            CloseHandle (handle);
    }
};

bool IsFile (const GS::UniString& path)
{
    const DWORD attributes = GetFileAttributesW ((LPCWSTR) path.ToUStr ().Get ());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ToWide (const GS::UniString& value)
{
    const wchar_t* const text = (const wchar_t*) value.ToUStr ().Get ();
    return text == nullptr ? std::wstring () : std::wstring (text);
}

GS::UniString LogPathFor (const GS::UniString& outputPath)
{
    const UIndex dot = outputPath.FindLast ('.');
    const UIndex separator = outputPath.FindLast ('\\');
    if (dot != MaxUIndex && (separator == MaxUIndex || dot > separator))
        return outputPath.GetSubstring (0, dot) + GS::UniString (".log");
    return outputPath + GS::UniString (".log");
}

bool FileHasData (const GS::UniString& path)
{
    const HANDLE file = CreateFileW ((LPCWSTR) path.ToUStr ().Get (), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = {};
    const bool readable = GetFileSizeEx (file, &size) != 0;
    CloseHandle (file);
    return readable && size.QuadPart > 0;
}

void RemovePartialOutput (const GS::UniString& path)
{
    DeleteFileW ((LPCWSTR) path.ToUStr ().Get ());
}

} // namespace

CloudCompareResult RunCloudCompareCli (const GS::UniString& executablePath, const GS::UniString& inputPath,
                                       const GS::UniString& outputPath, const double* cropPolygon,
                                       size_t cropPolygonCount, bool keepOutside, double subsampleStep,
                                       uint64_t runGeneration)
{
    CloudCompareResult result;
    result.outputPath = outputPath;

    if (!IsFile (executablePath)) {
        result.error =
            "CloudCompare " + GS::UniString (kCloudComparePinnedVersion) + " executable is missing: " + executablePath;
        return result;
    }
    if (!IsFile (inputPath)) {
        result.error = "CloudCompare input tile is missing: " + inputPath;
        return result;
    }
    if (cropPolygonCount > 0 && cropPolygon == nullptr) {
        result.error = "CloudCompare crop polygon count is non-zero but its point array is null.";
        return result;
    }
    if (subsampleStep < 0.0) {
        result.error = "CloudCompare subsample step cannot be negative.";
        return result;
    }

    // A stale PLY can make a failed process look successful to a later loader.
    RemovePartialOutput (outputPath);
    const GS::UniString logPath = LogPathFor (outputPath);
    result.logPath = logPath;

    CloudCompareCommandRequest request;
    request.executablePath = ToWide (executablePath);
    request.logPath = ToWide (logPath);
    request.inputPath = ToWide (inputPath);
    request.outputPath = ToWide (outputPath);
    request.keepOutside = keepOutside;
    request.subsampleStep = subsampleStep;
    request.cropPolygon.reserve (cropPolygonCount);
    for (size_t i = 0; i < cropPolygonCount; ++i)
        request.cropPolygon.push_back ({ cropPolygon[i * 2], cropPolygon[i * 2 + 1] });

    const std::wstring commandLine = BuildCloudCompareCommandLine (request);
    std::vector<wchar_t> mutableCommandLine (commandLine.begin (), commandLine.end ());
    mutableCommandLine.push_back (L'\0');

    Pipe output;
    if (!output.Create ()) {
        result.error = "Could not create the CloudCompare output pipe.";
        return result;
    }

    SECURITY_ATTRIBUTES inputAttributes = {};
    inputAttributes.nLength = sizeof (inputAttributes);
    inputAttributes.bInheritHandle = TRUE;
    const HANDLE nullInput = CreateFileW (L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inputAttributes,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) {
        result.error = "Could not open NUL for CloudCompare stdin.";
        return result;
    }

    Job job;
    job.handle = CreateJobObjectW (nullptr, nullptr);
    if (job.handle == nullptr) {
        CloseHandle (nullInput);
        result.error = GS::UniString::Printf ("Could not create the CloudCompare Job Object (Win32 error %u).",
                                              (unsigned) GetLastError ());
        return result;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject (job.handle, JobObjectExtendedLimitInformation, &limits, sizeof (limits))) {
        CloseHandle (nullInput);
        result.error = GS::UniString::Printf ("Could not configure the CloudCompare Job Object (Win32 error %u).",
                                              (unsigned) GetLastError ());
        return result;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof (startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullInput;
    startup.hStdOutput = output.write;
    startup.hStdError = output.write;
    PROCESS_INFORMATION process = {};
    const BOOL started = CreateProcessW ((LPCWSTR) executablePath.ToUStr ().Get (), mutableCommandLine.data (), nullptr,
                                         nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle (nullInput);
    if (!started) {
        result.error = GS::UniString::Printf ("Could not start pinned CloudCompare %T (Win32 error %u).",
                                              executablePath.ToPrintf (), (unsigned) GetLastError ());
        return result;
    }

    output.CloseWrite ();
    if (!AssignProcessToJobObject (job.handle, process.hProcess)) {
        const DWORD error = GetLastError ();
        TerminateProcess (process.hProcess, 1);
        WaitForSingleObject (process.hProcess, 5000);
        CloseHandle (process.hThread);
        CloseHandle (process.hProcess);
        result.error = GS::UniString::Printf ("Could not place CloudCompare in its Job Object (Win32 error %u).",
                                              (unsigned) error);
        return result;
    }

    std::string transcript;
    bool exited = false;
    bool killed = false;
    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe (output.read, nullptr, 0, nullptr, &available, nullptr) == 0)
            break;

        if (available > 0) {
            char buffer[4096];
            DWORD read = 0;
            const DWORD want = (available < sizeof (buffer)) ? available : (DWORD) sizeof (buffer);
            if (ReadFile (output.read, buffer, want, &read, nullptr) == 0 || read == 0)
                break;
            transcript.append (buffer, read);
            continue;
        }

        if (exited)
            break;
        if (WaitForSingleObject (process.hProcess, 0) == WAIT_OBJECT_0) {
            exited = true;
            continue;
        }

        if (!killed && RunCancel::Get ().IsCancelled (runGeneration)) {
            const BOOL terminated = TerminateProcess (process.hProcess, CloudCompareCancelExitCode);
            killed = true;
            if (terminated != 0)
                result.cancelled = true;
            else
                result.error = GS::UniString::Printf ("Could not cancel CloudCompare (Win32 error %u).",
                                                      (unsigned) GetLastError ());
            if (terminated == 0)
                break; // the Job Object kills it when this function unwinds
        }
        Sleep (20);
    }

    output.CloseRead ();
    WaitForSingleObject (process.hProcess, 5000);
    DWORD exitCode = 0;
    if (!GetExitCodeProcess (process.hProcess, &exitCode))
        result.error = GS::UniString::Printf ("Could not read the CloudCompare exit code (Win32 error %u).",
                                              (unsigned) GetLastError ());
    CloseHandle (process.hThread);
    CloseHandle (process.hProcess);
    result.transcript = GS::UniString (transcript.c_str (), CC_UTF8);
    result.exitCode = static_cast<int> (exitCode);

    if (result.cancelled || exitCode == CloudCompareCancelExitCode) {
        result.cancelled = true;
        RemovePartialOutput (outputPath);
        return result;
    }
    if (!result.error.IsEmpty ()) {
        RemovePartialOutput (outputPath);
        return result;
    }
    if (exitCode != 0) {
        result.error = GS::UniString::Printf ("Pinned CloudCompare %s exited with code %u.", kCloudComparePinnedVersion,
                                              (unsigned) exitCode);
        RemovePartialOutput (outputPath);
        return result;
    }
    if (!FileHasData (outputPath)) {
        result.error = "Pinned CloudCompare exited successfully but produced no non-empty PLY at " + outputPath;
        return result;
    }

    result.succeeded = true;
    return result;
}

} // namespace evp
