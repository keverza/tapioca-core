#include "VertexWeld.hpp"

#include <cmath>

namespace geomsrv {

namespace {

int32_t Quantize (double v)
{
    return static_cast<int32_t> (std::lround (v * kNormalQuantum));
}

} // namespace

uint32_t VertexWelder::Add (int32_t srcVertex, double x, double y, double z, double nx, double ny, double nz,
                            const double* uv)
{
    WeldKey key { srcVertex, Quantize (nx), Quantize (ny), Quantize (nz) };
    if (uv != nullptr) {
        key.hasUv = true;
        key.u = std::llround (uv[0] * 1.0e5);
        key.v = std::llround (uv[1] * 1.0e5);
    }

    const auto it = map_.find (key);
    if (it != map_.end ())
        return it->second;

    const uint32_t index = static_cast<uint32_t> (mesh_.VertexCount ());
    mesh_.vertices.push_back (x);
    mesh_.vertices.push_back (y);
    mesh_.vertices.push_back (z);
    mesh_.normals.push_back (static_cast<float> (nx));
    mesh_.normals.push_back (static_cast<float> (ny));
    mesh_.normals.push_back (static_cast<float> (nz));
    // #252: once any vertex has a coordinate, every vertex has two (zeros where it has none).
    // ⚠️ `uvs.empty ()` IS NOT "NO COORDINATES YET" ON ITS OWN: when the very first vertex is the
    // textured one, the back-fill is empty too, and testing emptiness alone skipped vertex 0's own
    // pair; the next vertex then back-filled it with (0, 0) and every triangle on vertex 0 drew
    // its texture squeezed (live, SimpleArchicadScene's terrain: 3 of 32 triangles, up to 24x).
    if (uv != nullptr || !mesh_.uvs.empty ()) {
        mesh_.uvs.resize (size_t (index) * 2, 0.0f); // back-fill the untextured vertices before it
        mesh_.uvs.push_back (uv != nullptr ? static_cast<float> (uv[0]) : 0.0f);
        mesh_.uvs.push_back (uv != nullptr ? static_cast<float> (uv[1]) : 0.0f);
    }
    map_.emplace (key, index);
    return index;
}

bool NormalizeVector (double& x, double& y, double& z)
{
    const double len = std::sqrt (x * x + y * y + z * z);
    if (!(len > 1e-12))
        return false;
    x /= len;
    y /= len;
    z /= len;
    return true;
}

void TriangleNormal (const double* a, const double* b, const double* c, double& nx, double& ny, double& nz)
{
    const double e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
    const double e2x = c[0] - a[0], e2y = c[1] - a[1], e2z = c[2] - a[2];
    nx = e1y * e2z - e1z * e2y;
    ny = e1z * e2x - e1x * e2z;
    nz = e1x * e2y - e1y * e2x;
}

void ComputeSmoothNormals (Mesh& mesh)
{
    const size_t nv = mesh.vertices.size ();
    mesh.normals.assign (nv, 0.0f);

    const auto& V = mesh.vertices;
    for (size_t t = 0; t + 2 < mesh.triangles.size (); t += 3) {
        const uint32_t a = mesh.triangles[t], b = mesh.triangles[t + 1], c = mesh.triangles[t + 2];
        double nx, ny, nz;
        TriangleNormal (&V[a * 3], &V[b * 3], &V[c * 3], nx, ny, nz);
        for (uint32_t idx : { a, b, c }) {
            mesh.normals[idx * 3] += static_cast<float> (nx);
            mesh.normals[idx * 3 + 1] += static_cast<float> (ny);
            mesh.normals[idx * 3 + 2] += static_cast<float> (nz);
        }
    }
    for (size_t v = 0; v + 2 < nv; v += 3) {
        double x = mesh.normals[v], y = mesh.normals[v + 1], z = mesh.normals[v + 2];
        if (NormalizeVector (x, y, z)) {
            mesh.normals[v] = static_cast<float> (x);
            mesh.normals[v + 1] = static_cast<float> (y);
            mesh.normals[v + 2] = static_cast<float> (z);
        }
    }
}

bool NormalsAreUsable (const Mesh& mesh)
{
    if (mesh.normals.size () != mesh.vertices.size ())
        return false;
    if (mesh.normals.empty ())
        return false;
    for (size_t v = 0; v + 2 < mesh.normals.size (); v += 3) {
        const double x = mesh.normals[v], y = mesh.normals[v + 1], z = mesh.normals[v + 2];
        const double lenSq = x * x + y * y + z * z;
        // A zero normal is unusable; anything not near unit length means the
        // producer gave us something we should not trust.
        if (lenSq < 0.9 * 0.9 || lenSq > 1.1 * 1.1)
            return false;
    }
    return true;
}

} // namespace geomsrv
