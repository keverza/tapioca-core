#ifndef GEOMETRYSERVER_SCREENSHOTSTORE_HPP
#define GEOMETRYSERVER_SCREENSHOTSTORE_HPP

#include <memory>
#include <mutex>
#include <string>
#include <cstdint>

// Holds the most recently captured PNG screenshots (raw file bytes). Screenshots
// are captured on Archicad's main thread (palette button) and served to HTTP
// worker threads from here — same immutable-publish pattern as MeshStore.
namespace geomsrv {

struct Shot {
    std::string png;         // raw PNG file bytes
    uint64_t    id = 0;      // increments each capture
};

class ScreenshotStore {
public:
    static ScreenshotStore& Get ()
    {
        static ScreenshotStore instance;
        return instance;
    }

    // Main thread: publish a freshly captured view. kind is "current" or "top".
    void Publish (const std::string& kind, std::string png)
    {
        std::lock_guard<std::mutex> lock (mtx);
        auto s = std::make_shared<Shot> ();
        s->png = std::move (png);
        s->id  = ++counter;
        if (kind == "top") top = s; else current = s;
    }

    // Any thread: fetch a view (null before first capture).
    std::shared_ptr<const Shot> Current () const { std::lock_guard<std::mutex> l (mtx); return current; }
    std::shared_ptr<const Shot> Top ()     const { std::lock_guard<std::mutex> l (mtx); return top; }

    void Release ()
    {
        std::lock_guard<std::mutex> l (mtx);
        current.reset ();
        top.reset ();
    }

    size_t Bytes () const
    {
        std::lock_guard<std::mutex> l (mtx);
        return (current ? current->png.capacity () : 0) + (top ? top->png.capacity () : 0);
    }

private:
    ScreenshotStore () = default;

    mutable std::mutex          mtx;
    std::shared_ptr<const Shot> current;
    std::shared_ptr<const Shot> top;
    uint64_t                    counter = 0;
};

} // namespace geomsrv

#endif
