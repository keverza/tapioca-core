#include "SunVisibility.hpp"

#include "ObjScene.hpp"

#include <nanort.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace renderbench {

namespace {

// Opaque as the shadow map counts it: kOpaqueAlpha in DiligentScene is a little under one.
constexpr float kCasterAlpha = 0.99f;
// N.L below this is lighting, not shadowing (the terminator); see kSunExcluded.
constexpr float kMinFacing = 0.05f;

struct Mesh {
    std::vector<float> vertices;
    std::vector<unsigned int> faces;
    std::vector<uint8_t> opaque; // per triangle (primary mesh only)
    nanort::BVHAccel<float> accel;

    void Build ()
    {
        if (faces.empty ())
            return;
        nanort::TriangleMesh<float> mesh (vertices.data (), faces.data (), sizeof (float) * 3);
        nanort::TriangleSAHPred<float> pred (vertices.data (), faces.data (), sizeof (float) * 3);
        accel.Build (unsigned (faces.size () / 3), mesh, pred);
    }

    bool Hit (const float org[3], const float dir[3], float minT, nanort::TriangleIntersection<float>& hit) const
    {
        if (faces.empty ())
            return false;
        nanort::Ray<float> ray;
        for (int k = 0; k < 3; ++k) {
            ray.org[k] = org[k];
            ray.dir[k] = dir[k];
        }
        ray.min_t = minT;
        ray.max_t = 1e30f;
        nanort::TriangleIntersector<float, nanort::TriangleIntersection<float>> intersector (
            vertices.data (), faces.data (), sizeof (float) * 3);
        return accel.Traverse (ray, intersector, &hit);
    }
};

void Normalize (float v[3])
{
    const float l = std::sqrt (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.0f)
        for (int k = 0; k < 3; ++k)
            v[k] /= l;
}

} // namespace

struct SunVisibility::Impl {
    Mesh all;     // every surface: what a primary ray sees
    Mesh casters; // opaque surfaces only: what blocks the sun
};

SunVisibility::SunVisibility () : impl_ (std::make_unique<Impl> ())
{
}
SunVisibility::~SunVisibility () = default;

void SunVisibility::Build (const ObjScene& scene)
{
    for (const auto& element : scene.elements) {
        for (const auto& range : element->ranges) {
            const bool opaque = scene.materials->Lookup (range.material).alpha >= kCasterAlpha;
            for (uint32_t i = 0; i + 2 < range.indexCount; i += 3) {
                float corner[9];
                for (int c = 0; c < 3; ++c) {
                    const uint32_t index = element->indices[range.firstIndex + i + c];
                    for (int k = 0; k < 3; ++k)
                        corner[c * 3 + k] = element->vertices[size_t (index) * 3 + k];
                }
                auto add = [&] (Mesh& m) {
                    const unsigned base = unsigned (m.vertices.size () / 3);
                    m.vertices.insert (m.vertices.end (), corner, corner + 9);
                    m.faces.insert (m.faces.end (), { base, base + 1, base + 2 });
                };
                add (impl_->all);
                impl_->all.opaque.push_back (opaque ? 1 : 0);
                if (opaque)
                    add (impl_->casters);
            }
        }
    }
    impl_->all.Build ();
    impl_->casters.Build ();
}

bool SunVisibility::Blocked (const double* p, const double* d, double near) const
{
    float origin[] = { float (p[0]), float (p[1]), float (p[2]) }, ray[] = { float (d[0]), float (d[1]), float (d[2]) };
    nanort::TriangleIntersection<float> hit;
    return impl_->casters.Hit (origin, ray, float (near), hit);
}

std::vector<uint8_t> SunVisibility::Render (const float eye[3], const float target[3], float fovDegrees, uint32_t width,
                                            uint32_t height, const float sunIn[3], float sunDiameterDegrees) const
{
    constexpr int kSoftSunRays = 64;
    std::vector<uint8_t> out (size_t (width) * height, kSunExcluded);
    float f[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
    Normalize (f);
    float r[3] = { f[1], -f[0], 0.0f }; // f x (0,0,1)
    Normalize (r);
    const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    const float tanY = std::tan (fovDegrees * 0.5f * 3.14159265f / 180.0f);
    const float tanX = tanY * float (width) / float (height);
    float sun[3] = { sunIn[0], sunIn[1], sunIn[2] };
    Normalize (sun);
    if (sunDiameterDegrees > 0.0f)
        std::fill (out.begin (), out.end (), kSoftSunExcluded);
    // The disk's plane: two axes perpendicular to the sun, and the tangent of its angular radius.
    float a[3] = { std::fabs (sun[2]) < 0.9f ? -sun[1] : 0.0f, std::fabs (sun[2]) < 0.9f ? sun[0] : -sun[2],
                   std::fabs (sun[2]) < 0.9f ? 0.0f : sun[1] };
    Normalize (a);
    const float b[3] = { sun[1] * a[2] - sun[2] * a[1], sun[2] * a[0] - sun[0] * a[2], sun[0] * a[1] - sun[1] * a[0] };
    const float sunTan = std::tan (sunDiameterDegrees * 0.5f * 3.14159265f / 180.0f);

    auto rows = [&] (uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const float sx = (2.0f * (float (x) + 0.5f) / float (width) - 1.0f) * tanX;
                const float sy = (1.0f - 2.0f * (float (y) + 0.5f) / float (height)) * tanY;
                float d[3];
                for (int k = 0; k < 3; ++k)
                    d[k] = f[k] + sx * r[k] + sy * u[k];
                Normalize (d);
                nanort::TriangleIntersection<float> hit;
                if (!impl_->all.Hit (eye, d, 0.0f, hit) || !impl_->all.opaque[hit.prim_id])
                    continue;
                const float* v = &impl_->all.vertices[size_t (hit.prim_id) * 9];
                const float e1[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] };
                const float e2[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
                float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                               e1[0] * e2[1] - e1[1] * e2[0] };
                Normalize (n);
                if (n[0] * d[0] + n[1] * d[1] + n[2] * d[2] > 0.0f)
                    for (float& c : n)
                        c = -c; // the side the camera sees
                if (n[0] * sun[0] + n[1] * sun[1] + n[2] * sun[2] < kMinFacing)
                    continue;
                // Off the surface by a hair, scaled with distance (float precision), never by a
                // texel: this is the exact answer the shadow map's biases approximate.
                const float lift = 1e-5f * (1.0f + hit.t);
                float p[3];
                for (int k = 0; k < 3; ++k)
                    p[k] = eye[k] + d[k] * hit.t + n[k] * lift;
                nanort::TriangleIntersection<float> blocker;
                if (sunDiameterDegrees <= 0.0f) {
                    out[size_t (y) * width + x] = impl_->casters.Hit (p, sun, 0.0f, blocker) ? kSunOccluded : kSunLit;
                    continue;
                }
                // The sun as a disk: kSoftSunRays directions spread evenly over it (a Vogel disk,
                // turned per pixel so the stratification does not band), the fraction unblocked.
                int visible = 0;
                const float turn = std::fmod (float (x) * 0.7548777f + float (y) * 0.5698403f, 1.0f) * 6.2831853f;
                for (int i = 0; i < kSoftSunRays; ++i) {
                    const float radius = sunTan * std::sqrt ((float (i) + 0.5f) / float (kSoftSunRays));
                    const float angle = float (i) * 2.39996323f + turn;
                    float s[3];
                    for (int k = 0; k < 3; ++k)
                        s[k] = sun[k] + radius * (std::cos (angle) * a[k] + std::sin (angle) * b[k]);
                    Normalize (s);
                    visible += impl_->casters.Hit (p, s, 0.0f, blocker) ? 0 : 1;
                }
                out[size_t (y) * width + x] =
                    uint8_t (std::lround (float (kSoftSunScale) * float (visible) / float (kSoftSunRays)));
            }
        }
    };
    const uint32_t threads = (std::max) (1u, (std::min) (std::thread::hardware_concurrency (), height));
    std::vector<std::thread> pool;
    for (uint32_t t = 0; t < threads; ++t)
        pool.emplace_back (rows, height * t / threads, height * (t + 1) / threads);
    for (std::thread& t : pool)
        t.join ();
    return out;
}

} // namespace renderbench
