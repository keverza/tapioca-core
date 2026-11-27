#include "ArchViz/DiligentSceneImpl.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/Camera.hpp"
#include "ArchViz/MatrixMath.hpp"
#include "ArchViz/SunPathScene.hpp"
#include <DirectXCollision.h>
#include <ranges>

namespace geomsrv::archviz {
namespace {
float SectionDistance (const CylinderPoint& p, const float plane[4])
{
    return p[0] * plane[0] + p[1] * plane[1] + p[2] * plane[2] + plane[3];
}

bool KeepSegment (CylinderSegment& s, const float plane[4])
{
    float a = SectionDistance (s[0], plane), b = SectionDistance (s[1], plane);
    if (a < 0 && b < 0)
        return false;
    if ((a < 0) != (b < 0)) {
        auto& end = s[a < 0 ? 0 : 1];
        for (int i = 0; i < 3; ++i)
            end[i] = s[0][i] + a / (a - b) * (s[1][i] - s[0][i]);
    }
    return s[0] != s[1];
}

// Test visibility at the candidate line point, rather than at the mouse pixel:
// the pick tolerance extends outside a wall at its silhouette.
bool Occluded (const CylinderPoint& point, const float eye[3], const float parallelDirection[3], bool parallel,
               const std::vector<Entry>& elements, const MaterialTable& materials, const ClayLook& look,
               const float plane[4])
{
    using namespace DirectX;
    auto load = [] (const float* v) { return XMVectorSet (v[0], v[1], v[2], 0); };
    auto target = load (point.data ()), origin = load (eye);
    auto direction = parallel ? load (parallelDirection) : XMVector3Normalize (target - origin);
    float distance = XMVectorGetX (XMVector3Dot (target - origin, direction));
    if (parallel)
        origin = target - direction * distance;
    for (const auto& entry : elements) {
        if (entry.hidden || !entry.sectionMesh || !entry.vertexBuffer || !entry.indexBuffer)
            continue;
        const auto& mesh = *entry.sectionMesh;
        BoundingBox box;
        BoundingBox::CreateFromPoints (box, load (mesh.boundsMin), load (mesh.boundsMax));
        float t;
        if (!box.Intersects (origin, direction, t) || t > distance)
            continue;
        for (const auto& range : entry.ranges) {
            if (DrawnAlpha (materials.Lookup (range.material), look) < kOpaqueAlpha)
                continue;
            for (size_t i = range.firstIndex;
                 i + 2 < size_t (range.firstIndex) + range.indexCount && i + 2 < mesh.indices.size (); i += 3) {
                if ((std::max) ({ mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2] }) >=
                    mesh.positions.size () / 3)
                    continue;
                auto vertex = [&] (size_t j) { return load (mesh.positions.data () + size_t (mesh.indices[j]) * 3); };
                if (TriangleTests::Intersects (origin, direction, vertex (i), vertex (i + 1), vertex (i + 2), t) &&
                    t < distance - (std::max) (.0001f, distance * 1e-6f)) {
                    XMFLOAT3 hit;
                    XMStoreFloat3 (&hit, origin + direction * t);
                    if (SectionDistance ({ hit.x, hit.y, hit.z }, plane) >= 0)
                        return true;
                }
            }
        }
    }
    return false;
}
} // namespace

void DiligentScene::DrawSunCylinder (Diligent::IDeviceContext* context, const float viewProj[16], uint32_t width,
                                     uint32_t height, uint32_t colorFormat, uint32_t depthFormat,
                                     const SunCylinder* cylinder)
{
    if (!impl_)
        return;
    auto& s = *impl_;
    if (!cylinder || !context || !s.ready) {
        s.cylinderKey = {};
        s.cylinderSegments.clear ();
        return;
    }
    auto visible = s.elements | std::views::filter ([] (const Entry& e) {
                       return !e.hidden && e.sectionMesh && e.vertexBuffer && e.indexBuffer;
                   }) |
                   std::views::transform (&Entry::sectionMesh);
    if (s.cylinderKey.Update (*cylinder, s.sectionPlane, visible)) {
        s.cylinderSegments.clear ();
        CylinderContour contour;
        for (const auto& mesh : s.cylinderKey.meshes)
            contour.Append (*mesh, *cylinder);
        if (contour.refused) {
            ArchVizLog ("Sun cylinder contour exceeded its segment limit");
            return;
        }
        GhPreviewDrawables lines;
        const auto orange = SunPathDisplayRgba (245, 158, 36, 220);
        for (auto segment : contour.segments) {
            if (!KeepSegment (segment, s.sectionPlane))
                continue;
            s.cylinderSegments.push_back (segment);
            AppendPreviewSegment (segment[0].data (), segment[1].data (), orange, lines.depthTested);
        }
        if (!s.cylinderSegments.empty ()) {
            std::string error;
            if ((!s.cylinderLayer.IsReady () && !s.cylinderLayer.Init (s.device, colorFormat, depthFormat, error)) ||
                !s.cylinderLayer.Upload (s.device, context, lines, error)) {
                ArchVizLog ("Sun cylinder contour unavailable: " + error);
                s.cylinderSegments.clear ();
            }
        }
    }
    if (s.cylinderSegments.empty ())
        return;
    DrawOcclusionDepth (context, viewProj, true, true, colorFormat, depthFormat);
    GhPreviewLayer::DrawParams params;
    params.squareCaps = false;
    params.lineWidthPixels = 2.f;
    s.cylinderLayer.Draw (context, viewProj, width, height, params);
}

bool DiligentScene::HitSunCylinder (const Camera& camera, const float viewProj[16], uint32_t width, uint32_t height,
                                    int x, int y) const
{
    if (!impl_ || !width || !height)
        return false;
    float eye[3], direction[3];
    camera.CursorRay (x, y, width, height, eye, direction);
    // Apply the camera's homogeneous near/far tests to the segment, not its
    // endpoints individually: a tall wall can cross the near plane during orbit.
    const float nearPlane[4] = { viewProj[2], viewProj[6], viewProj[10], viewProj[14] };
    const float farPlane[4] = { viewProj[3] - viewProj[2], viewProj[7] - viewProj[6], viewProj[11] - viewProj[10],
                                viewProj[15] - viewProj[14] };
    for (auto segment : impl_->cylinderSegments) {
        if (!KeepSegment (segment, nearPlane) || !KeepSegment (segment, farPlane))
            continue;
        float px[2], py[2], w[2];
        bool visible = true;
        for (int i = 0; i < 2; ++i) {
            const auto& p = segment[i];
            float v[] = { p[0], p[1], p[2], 1.f }, clip[4];
            TransformPoint (clip, v, viewProj);
            w[i] = clip[3];
            if (w[i] <= 0) {
                visible = false;
                break;
            }
            px[i] = (1 + clip[0] / w[i]) * width * .5f;
            py[i] = (1 - clip[1] / w[i]) * height * .5f;
        }
        if (!visible)
            continue;
        float dx = px[1] - px[0], dy = py[1] - py[0], length2 = dx * dx + dy * dy;
        float t = length2 > 0 ? std::clamp (((x - px[0]) * dx + (y - py[0]) * dy) / length2, 0.f, 1.f) : 0;
        if (std::hypot (px[0] + t * dx - x, py[0] + t * dy - y) > 5)
            continue;
        // Perspective-correct interpolation back to the point on the scene.
        t = t * w[0] / ((1 - t) * w[1] + t * w[0]);
        CylinderPoint point;
        for (int i = 0; i < 3; ++i)
            point[i] = segment[0][i] + t * (segment[1][i] - segment[0][i]);
        if (!Occluded (point, eye, direction, camera.IsOrthographic (), impl_->elements, impl_->materials,
                       impl_->SurfaceLook (impl_->StudyModelShown ()), impl_->sectionPlane))
            return true;
    }
    return false;
}
} // namespace geomsrv::archviz
