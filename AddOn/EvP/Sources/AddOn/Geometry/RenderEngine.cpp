#include "RenderEngine.hpp"

#include <cmath>
#include <thread>
#include <algorithm>

namespace geomsrv {

namespace {

inline void  Sub (const double a[3], const double b[3], double o[3]) { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
inline double Dot (const double a[3], const double b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline void  Cross (const double a[3], const double b[3], double o[3]) {
    o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}
inline void Norm (double v[3]) {
    const double l = std::sqrt (Dot (v, v));
    if (l > 0.0) { v[0]/=l; v[1]/=l; v[2]/=l; }
}

} // namespace

RenderResult Render (const QueryEngine& eng, const Camera& cam, int w, int h, unsigned threads)
{
    RenderResult out;
    if (w <= 0 || h <= 0)
        return out;
    out.w = w; out.h = h;
    out.id.assign (static_cast<size_t> (w) * h, -1);
    out.depth.assign (static_cast<size_t> (w) * h, 0.0f);
    out.normal.assign (static_cast<size_t> (w) * h * 3, 0.0f);

    // Build the camera basis: forward, right, trueUp (right-handed, view down -forward).
    double fwd[3]; Sub (cam.target, cam.eye, fwd); Norm (fwd);
    double right[3]; Cross (fwd, cam.up, right); Norm (right);
    double tup[3]; Cross (right, fwd, tup);   // already unit (fwd,right orthonormal)

    const double aspect = static_cast<double> (w) / static_cast<double> (h);
    const double halfH = cam.ortho ? cam.orthoHeight * 0.5
                                   : std::tan (cam.fovYdeg * 0.5 * 3.14159265358979323846 / 180.0);
    const double halfW = halfH * aspect;

    auto renderRows = [&] (int r0, int r1) {
        for (int r = r0; r < r1; ++r) {
            const double ndcY = 1.0 - (r + 0.5) / h * 2.0;   // +1 top .. -1 bottom
            for (int c = 0; c < w; ++c) {
                const double ndcX = (c + 0.5) / w * 2.0 - 1.0; // -1 left .. +1 right
                double org[3], dir[3];
                if (cam.ortho) {
                    for (int k = 0; k < 3; ++k)
                        org[k] = cam.eye[k] + ndcX * halfW * right[k] + ndcY * halfH * tup[k];
                    dir[0] = fwd[0]; dir[1] = fwd[1]; dir[2] = fwd[2];
                } else {
                    for (int k = 0; k < 3; ++k) {
                        org[k] = cam.eye[k];
                        dir[k] = fwd[k] + ndcX * halfW * right[k] + ndcY * halfH * tup[k];
                    }
                }
                const auto hit = eng.Raycast (org, dir, 0.0);
                if (!hit.hit)
                    continue;
                const size_t px = static_cast<size_t> (r) * w + c;
                out.id[px] = static_cast<int32_t> (hit.meshIndex);
                out.depth[px] = static_cast<float> (hit.t);
                out.normal[px * 3 + 0] = static_cast<float> (hit.normal[0]);
                out.normal[px * 3 + 1] = static_cast<float> (hit.normal[1]);
                out.normal[px * 3 + 2] = static_cast<float> (hit.normal[2]);
            }
        }
    };

    if (threads == 0)
        threads = std::max (1u, std::thread::hardware_concurrency ());
    threads = std::min<unsigned> (threads, static_cast<unsigned> (h));

    if (threads <= 1) {
        renderRows (0, h);
    } else {
        std::vector<std::thread> pool;
        const int chunk = (h + static_cast<int> (threads) - 1) / static_cast<int> (threads);
        for (unsigned t = 0; t < threads; ++t) {
            const int r0 = static_cast<int> (t) * chunk;
            const int r1 = std::min (h, r0 + chunk);
            if (r0 >= r1) break;
            pool.emplace_back (renderRows, r0, r1);
        }
        for (auto& th : pool) th.join ();
    }
    return out;
}

} // namespace geomsrv
