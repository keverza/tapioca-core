// The Win32 half of IScriptWatcher: notice that VSCode or Sublime saved a file.
//
// ⚠️ EXCLUDED FROM THE OFFLINE C++ SUITE, DELIBERATELY, AND IT IS THE ONLY FILE
// IN NodeGraph/ THAT IS. Everything else here is portable so the whole runtime is
// covered without Archicad; this one includes <windows.h>, so it stays out of
// EVP_NODEGRAPH_SOURCES exactly as ArchicadHostImpl.cpp does. What it costs is
// that the debounce below is not covered offline. What it buys is that nothing
// ELSE had to become platform code: the watcher hands a path to a callback, and
// every decision that follows is in ScriptReload, which is tested.
//
// ⚠️ IT WATCHES DIRECTORIES, NOT FILES, AND THAT IS NOT LAZINESS. An editor saves
// by writing a temporary file and renaming it over the target. A handle opened on
// the target is a handle on a file that ceases to exist mid-save - so a per-file
// watch stops watching at precisely the moment it was installed to fire.
//
// ⚠️ AND ONE SAVE IS SEVERAL NOTIFICATIONS. The temporary appears, it is renamed,
// the attributes are written; ReadDirectoryChangesW reports each. Reloading three
// times per save is not merely wasteful - each reload can reshape the node's
// ports, so the user would watch their node flicker. The debounce below collapses
// a burst into one reload, and the quiet period is longer than a save and shorter
// than a person's reaction time.

#include "NodeGraph/ScriptSource.hpp"

// ⚠️ NOMINMAX BEFORE <windows.h>, ALWAYS. Without it the header defines `min`
// and `max` as macros, and the next `std::min` in this file becomes a syntax
// error pointing at a line that is perfectly correct C++.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace evp::nodegraph {
namespace {

// Long enough to swallow an editor's save sequence, short enough that the reload
// still feels like part of pressing Ctrl+S.
constexpr auto kQuietPeriod = std::chrono::milliseconds (150);

std::wstring Widen (const std::string& utf8)
{
    if (utf8.empty ())
        return {};
    const int length = MultiByteToWideChar (CP_UTF8, 0, utf8.c_str (), static_cast<int> (utf8.size ()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring wide (static_cast<size_t> (length), L'\0');
    MultiByteToWideChar (CP_UTF8, 0, utf8.c_str (), static_cast<int> (utf8.size ()), wide.data (), length);
    return wide;
}

std::string Narrow (const std::wstring& wide)
{
    if (wide.empty ())
        return {};
    const int length =
        WideCharToMultiByte (CP_UTF8, 0, wide.c_str (), static_cast<int> (wide.size ()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string utf8 (static_cast<size_t> (length), '\0');
    WideCharToMultiByte (CP_UTF8, 0, wide.c_str (), static_cast<int> (wide.size ()), utf8.data (), length, nullptr,
                         nullptr);
    return utf8;
}

std::wstring DirectoryOf (const std::wstring& path)
{
    const size_t separator = path.find_last_of (L"\\/");
    return separator == std::wstring::npos ? std::wstring {} : path.substr (0, separator);
}

// One watched directory: its handle, its overlapped read, and its buffer.
struct WatchedDirectory {
    std::wstring directory;
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped {};

    // DWORD-aligned, as ReadDirectoryChangesW requires: FILE_NOTIFY_INFORMATION
    // is read out of it by pointer arithmetic, and a misaligned buffer is
    // undefined behaviour rather than a slow one.
    alignas (DWORD) unsigned char buffer[16 * 1024] {};
};

class Win32ScriptWatcher final : public IScriptWatcher {
  public:
    explicit Win32ScriptWatcher (ScriptChangeCallback onChange) : onChange_ (std::move (onChange))
    {
        // Manual-reset and initially unsignalled. It is what makes both a
        // rewatch and a stop able to interrupt the wait: the thread waits on the
        // directory events AND on this one.
        wakeUp_ = CreateEventW (nullptr, TRUE, FALSE, nullptr);
        thread_ = std::thread ([this] { Run (); });
    }

    ~Win32ScriptWatcher () override
    {
        Stop ();
        if (wakeUp_ != nullptr)
            CloseHandle (wakeUp_);
    }

    void WatchPaths (const std::vector<std::string>& paths) override
    {
        {
            const std::lock_guard<std::mutex> guard (mutex_);
            requested_.clear ();
            for (const std::string& path : paths) {
                const std::wstring directory = DirectoryOf (Widen (path));
                if (!directory.empty ())
                    requested_.push_back (directory);
            }
            std::sort (requested_.begin (), requested_.end ());
            requested_.erase (std::unique (requested_.begin (), requested_.end ()), requested_.end ());
            rewatch_ = true;
        }
        SetEvent (wakeUp_);
    }

    void Stop () override
    {
        if (stopping_.exchange (true))
            return; // Idempotent: a destructor after an explicit Stop must not join twice.
        if (wakeUp_ != nullptr)
            SetEvent (wakeUp_);
        if (thread_.joinable ())
            thread_.join ();
    }

  private:
    void Run ()
    {
        std::vector<std::unique_ptr<WatchedDirectory>> watched;
        // path -> when its quiet period expires. A save that arrives while an
        // earlier one is still cooling simply pushes the deadline out, which is
        // the behaviour that collapses a burst.
        std::map<std::string, std::chrono::steady_clock::time_point> pending;

        while (!stopping_.load ()) {
            if (rewatch_.exchange (false))
                Rewatch (watched);

            std::vector<HANDLE> handles;
            handles.push_back (wakeUp_);
            for (const auto& directory : watched)
                handles.push_back (directory->overlapped.hEvent);

            // Wake for the earliest pending deadline, so a burst that stops
            // arriving still fires. Without it a save whose last notification
            // has already been consumed would sit in `pending` until the next
            // unrelated filesystem event.
            DWORD timeout = INFINITE;
            if (!pending.empty ()) {
                const auto now = std::chrono::steady_clock::now ();
                auto earliest = pending.begin ()->second;
                for (const auto& [path, deadline] : pending)
                    earliest = std::min (earliest, deadline);
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (earliest - now);
                timeout = remaining.count () <= 0 ? 0 : static_cast<DWORD> (remaining.count ());
            }

            const DWORD signalled =
                WaitForMultipleObjects (static_cast<DWORD> (handles.size ()), handles.data (), FALSE, timeout);
            if (stopping_.load ())
                break;

            if (signalled == WAIT_OBJECT_0) {
                ResetEvent (wakeUp_);
            }
            else if (signalled > WAIT_OBJECT_0 && signalled < WAIT_OBJECT_0 + handles.size ()) {
                const size_t index = signalled - WAIT_OBJECT_0 - 1;
                CollectChanges (*watched[index], pending);
                Rearm (*watched[index]);
            }

            const auto now = std::chrono::steady_clock::now ();
            for (auto entry = pending.begin (); entry != pending.end ();) {
                if (entry->second > now) {
                    ++entry;
                    continue;
                }
                // ⚠️ THE CALLBACK RUNS ON THIS THREAD. It must not touch the
                // document, the evaluator or DG - see ScriptSource.hpp. It is
                // called with no lock held, because whatever it marshals to may
                // well come back and ask this watcher to rewatch.
                if (onChange_)
                    onChange_ (entry->first);
                entry = pending.erase (entry);
            }
        }

        for (const auto& directory : watched)
            Close (*directory);
    }

    void Rewatch (std::vector<std::unique_ptr<WatchedDirectory>>& watched)
    {
        std::vector<std::wstring> wanted;
        {
            const std::lock_guard<std::mutex> guard (mutex_);
            wanted = requested_;
        }

        // Directories already watched are LEFT ALONE rather than closed and
        // reopened. Rewatching happens on every graph edit that touches a script
        // node, and tearing down a live handle each time would leave a window in
        // which a save goes unnoticed.
        std::erase_if (watched, [&] (const std::unique_ptr<WatchedDirectory>& directory) {
            if (std::find (wanted.begin (), wanted.end (), directory->directory) != wanted.end ())
                return false;
            Close (*directory);
            return true;
        });

        for (const std::wstring& directory : wanted) {
            const bool already =
                std::any_of (watched.begin (), watched.end (), [&] (const std::unique_ptr<WatchedDirectory>& existing) {
                    return existing->directory == directory;
                });
            if (already)
                continue;
            auto entry = std::make_unique<WatchedDirectory> ();
            entry->directory = directory;
            entry->handle = CreateFileW (directory.c_str (), FILE_LIST_DIRECTORY,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (entry->handle == INVALID_HANDLE_VALUE)
                continue; // A folder that has been deleted or is not readable. The node still reloads on evaluate.
            entry->overlapped.hEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
            if (entry->overlapped.hEvent == nullptr) {
                CloseHandle (entry->handle);
                continue;
            }
            if (!Rearm (*entry)) {
                Close (*entry);
                continue;
            }
            watched.push_back (std::move (entry));
        }
    }

    static bool Rearm (WatchedDirectory& directory)
    {
        // Not recursive: a script node names one file, so its own folder is the
        // whole interesting set. A recursive watch on a folder someone happened
        // to put a script in - a repository root, a home directory - would
        // deliver every unrelated write on the machine to this thread.
        return ReadDirectoryChangesW (directory.handle, directory.buffer, sizeof (directory.buffer), FALSE,
                                      FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                                          FILE_NOTIFY_CHANGE_FILE_NAME,
                                      nullptr, &directory.overlapped, nullptr) != 0;
    }

    void CollectChanges (WatchedDirectory& directory,
                         std::map<std::string, std::chrono::steady_clock::time_point>& pending) const
    {
        DWORD transferred = 0;
        if (!GetOverlappedResult (directory.handle, &directory.overlapped, &transferred, FALSE))
            return;
        // Zero means the buffer overflowed and the changes were lost. Reported to
        // every watched path rather than dropped: a missed save is the one
        // failure this whole file exists to prevent, and a redundant reload is
        // cheap. RefreshDiskStamps on the other side filters out the files that
        // did not actually change.
        const auto deadline = std::chrono::steady_clock::now () + kQuietPeriod;
        if (transferred == 0) {
            pending.insert_or_assign (Narrow (directory.directory), deadline);
            return;
        }

        size_t offset = 0;
        while (offset + sizeof (FILE_NOTIFY_INFORMATION) <= transferred) {
            const auto* notification = reinterpret_cast<const FILE_NOTIFY_INFORMATION*> (directory.buffer + offset);
            const std::wstring name (notification->FileName, notification->FileNameLength / sizeof (WCHAR));
            if (!name.empty ())
                pending.insert_or_assign (Narrow (directory.directory + L"\\" + name), deadline);
            if (notification->NextEntryOffset == 0)
                break;
            offset += notification->NextEntryOffset;
        }
    }

    static void Close (WatchedDirectory& directory)
    {
        if (directory.handle != INVALID_HANDLE_VALUE) {
            // Cancel before closing: an outstanding overlapped read writes into
            // the buffer, and the buffer is about to be freed.
            CancelIoEx (directory.handle, &directory.overlapped);
            CloseHandle (directory.handle);
            directory.handle = INVALID_HANDLE_VALUE;
        }
        if (directory.overlapped.hEvent != nullptr) {
            CloseHandle (directory.overlapped.hEvent);
            directory.overlapped.hEvent = nullptr;
        }
    }

    ScriptChangeCallback onChange_;
    std::thread thread_;
    HANDLE wakeUp_ = nullptr;
    std::atomic<bool> stopping_ { false };
    std::atomic<bool> rewatch_ { false };

    mutable std::mutex mutex_;
    std::vector<std::wstring> requested_;
};

} // namespace

std::unique_ptr<IScriptWatcher> MakeWin32ScriptWatcher (ScriptChangeCallback onChange)
{
    return std::make_unique<Win32ScriptWatcher> (std::move (onChange));
}

} // namespace evp::nodegraph
