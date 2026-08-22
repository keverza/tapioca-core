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

    // Any thread: publish immutable PNG bytes. kind is current, top, or diligent.
    void Publish (const std::string& kind, std::string png, uint64_t id = 0)
    {
        std::lock_guard<std::mutex> lock (mtx);
        auto s = std::make_shared<Shot> ();
        s->png = std::move (png);
        s->id  = id != 0 ? id : ++counter;
        if (kind == "top")
            top = s;
        else if (kind == "diligent")
            diligent = s;
        else
            current = s;
    }

    // Any thread: fetch a view (null before first capture).
    std::shared_ptr<const Shot> Current () const { std::lock_guard<std::mutex> l (mtx); return current; }
    std::shared_ptr<const Shot> Top ()     const { std::lock_guard<std::mutex> l (mtx); return top; }
    std::shared_ptr<const Shot> Diligent () const { std::lock_guard<std::mutex> l (mtx); return diligent; }

    void Release ()
    {
        std::lock_guard<std::mutex> l (mtx);
        current.reset ();
        top.reset ();
        diligent.reset ();
    }

    size_t Bytes () const
    {
        std::lock_guard<std::mutex> l (mtx);
        return (current ? current->png.capacity () : 0) + (top ? top->png.capacity () : 0) +
               (diligent ? diligent->png.capacity () : 0);
    }

private:
    ScreenshotStore () = default;

    mutable std::mutex          mtx;
    std::shared_ptr<const Shot> current;
    std::shared_ptr<const Shot> top;
    std::shared_ptr<const Shot> diligent;
    uint64_t                    counter = 0;
};

} // namespace geomsrv

#endif
