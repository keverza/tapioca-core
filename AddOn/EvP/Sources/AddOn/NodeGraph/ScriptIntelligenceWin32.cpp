// The Win32 half of ILanguageServerProcess: start a program and own its pipes.
//
// ⚠️ EXCLUDED FROM THE OFFLINE C++ SUITE, exactly as ScriptWatcherWin32.cpp is
// and for the same reason: it includes <windows.h>, so it stays out of
// EVP_NODEGRAPH_SOURCES. What that costs is that process creation is not covered
// offline. What it buys is that nothing ELSE had to become platform code - the
// framing, the request routing, the URI encoding, the UTF-16 column mapping and
// the completion parsing are all in ScriptIntelligence.cpp, and all of those are
// tested.
//
// ⚠️ EVERY HANDLE HERE IS OWNED, AND A LEAKED ONE IS NOT A LEAK, IT IS A HANG.
// The child inherits the WRITE end of its stdout pipe; if this process also
// keeps a copy open, the read end never sees EOF and a `Read` after the server
// exits waits forever on a pipe nobody will ever write to again. So the
// inheritable ends are closed immediately after CreateProcess, before a single
// byte is read.
//
// ⚠️ AND THE READS ARE NON-BLOCKING BY CONSTRUCTION, NOT BY FLAG. An anonymous
// pipe cannot be opened in overlapped mode, and there is no timeout on ReadFile
// for one - so the only safe way to poll is PeekNamedPipe, which reports how much
// is available and reads exactly that. A plain ReadFile here would block the
// calling thread until the server happened to say something, which for a language
// server that has finished answering is "never".

#include "NodeGraph/ScriptIntelligence.hpp"

// ⚠️ NOMINMAX BEFORE <windows.h>, ALWAYS. Without it the header defines `min`
// and `max` as macros and the next `std::min` becomes a syntax error on a line
// that is perfectly correct C++.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace evp::nodegraph {
namespace {

std::wstring WideFromUtf8 (const std::string& text)
{
    if (text.empty ())
        return std::wstring ();
    const int length = MultiByteToWideChar (CP_UTF8, 0, text.c_str (), static_cast<int> (text.size ()), nullptr, 0);
    if (length <= 0)
        return std::wstring ();
    std::wstring wide (static_cast<size_t> (length), L'\0');
    MultiByteToWideChar (CP_UTF8, 0, text.c_str (), static_cast<int> (text.size ()), wide.data (), length);
    return wide;
}

/**
 * One argument, quoted the way CommandLineToArgvW will take it apart again.
 *
 * ⚠️ THE BACKSLASH RULE IS NOT OPTIONAL AND IT IS NOT INTUITIVE. Windows has no
 * argv; a command line is one string and every program re-splits it. Backslashes
 * are literal EXCEPT immediately before a quote, where they are escapes - so a
 * path ending in a separator, which is exactly what a folder argument looks like,
 * turns its own closing quote into a literal one and swallows the next argument.
 * The runtime path here comes from %LOCALAPPDATA% and may contain spaces, so
 * quoting is not avoidable either.
 */
std::wstring QuoteArgument (const std::wstring& argument)
{
    if (!argument.empty () && argument.find_first_of (L" \t\"") == std::wstring::npos)
        return argument;

    std::wstring quoted = L"\"";
    for (size_t index = 0; index < argument.size (); ++index) {
        size_t slashes = 0;
        while (index < argument.size () && argument[index] == L'\\') {
            ++index;
            ++slashes;
        }
        if (index == argument.size ()) {
            // Trailing backslashes precede the CLOSING quote, so they are escapes
            // and each needs doubling.
            quoted.append (slashes * 2, L'\\');
            break;
        }
        if (argument[index] == L'"') {
            quoted.append (slashes * 2 + 1, L'\\');
            quoted.push_back (L'"');
        }
        else {
            quoted.append (slashes, L'\\');
            quoted.push_back (argument[index]);
        }
    }
    quoted.push_back (L'"');
    return quoted;
}

class Win32LanguageServerProcess : public ILanguageServerProcess {
  public:
    Win32LanguageServerProcess (HANDLE process, HANDLE stdinWrite, HANDLE stdoutRead)
        : process_ (process), stdinWrite_ (stdinWrite), stdoutRead_ (stdoutRead)
    {
    }

    ~Win32LanguageServerProcess () override
    {
        Stop ();
    }

    bool Running () const override
    {
        if (process_ == nullptr)
            return false;
        return WaitForSingleObject (process_, 0) == WAIT_TIMEOUT;
    }

    bool Write (const std::string& bytes) override
    {
        if (stdinWrite_ == nullptr)
            return false;
        size_t written = 0;
        while (written < bytes.size ()) {
            DWORD chunk = 0;
            const DWORD remaining = static_cast<DWORD> (bytes.size () - written);
            if (!WriteFile (stdinWrite_, bytes.data () + written, remaining, &chunk, nullptr) || chunk == 0)
                return false;
            written += chunk;
        }
        return true;
    }

    void Read (std::string& into) override
    {
        if (stdoutRead_ == nullptr)
            return;
        for (;;) {
            DWORD available = 0;
            // PeekNamedPipe rather than ReadFile: see the header note. A failure
            // here means the pipe is gone, which is the server having exited.
            if (!PeekNamedPipe (stdoutRead_, nullptr, 0, nullptr, &available, nullptr))
                return;
            if (available == 0)
                return;
            std::string chunk (available, '\0');
            DWORD read = 0;
            if (!ReadFile (stdoutRead_, chunk.data (), available, &read, nullptr) || read == 0)
                return;
            into.append (chunk.data (), read);
        }
    }

    void Stop () override
    {
        // Closing stdin first gives the server the chance to notice EOF and exit
        // on its own, which is how a language server is meant to be stopped. The
        // wait is short: this runs on Archicad's shutdown path, and a palette
        // that took five seconds to close because of a subprocess would be a
        // worse bug than an orphaned one.
        if (stdinWrite_ != nullptr) {
            CloseHandle (stdinWrite_);
            stdinWrite_ = nullptr;
        }
        if (process_ != nullptr) {
            if (WaitForSingleObject (process_, 1500) == WAIT_TIMEOUT)
                TerminateProcess (process_, 0);
            CloseHandle (process_);
            process_ = nullptr;
        }
        if (stdoutRead_ != nullptr) {
            CloseHandle (stdoutRead_);
            stdoutRead_ = nullptr;
        }
    }

  private:
    HANDLE process_ = nullptr;
    HANDLE stdinWrite_ = nullptr;
    HANDLE stdoutRead_ = nullptr;
};

std::unique_ptr<ILanguageServerProcess> StartWin32Process (const std::string& executable,
                                                           const std::vector<std::string>& arguments)
{
    if (executable.empty ())
        return nullptr;

    SECURITY_ATTRIBUTES inheritable {};
    inheritable.nLength = sizeof (inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe (&stdoutRead, &stdoutWrite, &inheritable, 0))
        return nullptr;
    // ⚠️ THE PARENT'S END MUST NOT BE INHERITABLE. If the child gets a copy of
    // the read end it is holding a handle to its own output, and nothing about
    // that is diagnosable later.
    SetHandleInformation (stdoutRead, HANDLE_FLAG_INHERIT, 0);

    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;
    if (!CreatePipe (&stdinRead, &stdinWrite, &inheritable, 0)) {
        CloseHandle (stdoutRead);
        CloseHandle (stdoutWrite);
        return nullptr;
    }
    SetHandleInformation (stdinWrite, HANDLE_FLAG_INHERIT, 0);

    std::wstring commandLine = QuoteArgument (WideFromUtf8 (executable));
    for (const std::string& argument : arguments) {
        commandLine.push_back (L' ');
        commandLine.append (QuoteArgument (WideFromUtf8 (argument)));
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.hStdInput = stdinRead;
    startup.hStdOutput = stdoutWrite;
    // stderr goes to the child's own stdout pipe rather than to a third one.
    // Anything the server writes there is diagnostics, not protocol, and it is
    // discarded by the framing reader - but a stderr with nowhere to go is a
    // pipe that fills and blocks the server mid-write.
    startup.hStdError = stdoutWrite;
    // ⚠️ NO CONSOLE WINDOW. Without this, starting a language server from inside
    // Archicad flashes a black box over the user's model.
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION information {};
    // The command line buffer must be WRITABLE: CreateProcessW is documented to
    // modify it in place.
    std::vector<wchar_t> mutableCommandLine (commandLine.begin (), commandLine.end ());
    mutableCommandLine.push_back (L'\0');

    const BOOL started = CreateProcessW (nullptr, mutableCommandLine.data (), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                         nullptr, nullptr, &startup, &information);

    // The child's ends, closed in the parent whether or not it started - see the
    // header's note on why a surviving copy turns into a hang rather than a leak.
    CloseHandle (stdoutWrite);
    CloseHandle (stdinRead);

    if (!started) {
        CloseHandle (stdoutRead);
        CloseHandle (stdinWrite);
        return nullptr;
    }
    // The thread handle is of no use here and is closed at once; the process
    // handle is what Running() and Stop() need.
    CloseHandle (information.hThread);

    return std::make_unique<Win32LanguageServerProcess> (information.hProcess, stdinWrite, stdoutRead);
}

} // namespace

void InstallWin32LanguageServerProcess ()
{
    SetLanguageServerProcessFactory (&StartWin32Process);
}

} // namespace evp::nodegraph
