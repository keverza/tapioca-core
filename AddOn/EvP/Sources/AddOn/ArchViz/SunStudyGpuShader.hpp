#ifndef EVP_ARCHVIZ_SUNSTUDYGPUSHADER_HPP
#define EVP_ARCHVIZ_SUNSTUDYGPUSHADER_HPP

namespace geomsrv::archviz {

// FP64 basic operations work on SM5 devices without DXR/RT cores. Direction
// divisions/normalisation are done once in double precision on the CPU. The
// triangle test is nanort's watertight shear test, with scaled distance bounds
// instead of per-ray double division. Result 2 requests exact CPU resolution,
// also when the bounded node/triangle work allowance is exhausted.
inline constexpr char kSunStudyGpuShader[] = R"hlsl(
struct Node { double3 lo; double3 hi; uint first; uint count; uint escape; uint pad; };
struct Triangle { double3 a; double3 b; double3 c; };
StructuredBuffer<Node> nodes : register(t0);
StructuredBuffer<Triangle> triangles : register(t1);
StructuredBuffer<double3> origins : register(t2);
RWStructuredBuffer<uint> answers : register(u0);
cbuffer Parameters : register(b0) {
    double4 inverseAndMin;
    double4 shearAndMax;
    uint4 axesAndCount;
    uint4 sceneAndFlags;
    double4 guard;
};
double Magnitude(double x) { return x < 0.0 ? -x : x; }
// SM5 cannot dynamically address FP64 vector components. Explicit selection
// keeps the BVH/leaf loops dynamic instead of forcing FXC to unroll the scene.
double Component(double3 v, uint axis) {
    if (axis == 0) return v.x;
    if (axis == 1) return v.y;
    return v.z;
}
bool BoxHit(Node node, double3 origin) {
    double nearT = inverseAndMin.w;
    double farT = shearAndMax.w;
    [unroll] for (uint axis = 0; axis < 3; ++axis) {
        double lo = node.lo[axis] - guard.x;
        double hi = node.hi[axis] + guard.x;
        if ((sceneAndFlags.y & (1u << axis)) != 0) {
            if (origin[axis] < lo || origin[axis] > hi) return false;
        } else {
            precise double a = (lo - origin[axis]) * inverseAndMin[axis];
            precise double b = (hi - origin[axis]) * inverseAndMin[axis];
            if (a > b) { double swap = a; a = b; b = swap; }
            nearT = a > nearT ? a : nearT;
            farT = b < farT ? b : farT;
            if (nearT > farT) return false;
        }
    }
    return true;
}
uint TriangleHit(Triangle tri, double3 origin) {
    precise double3 A = tri.a - origin;
    precise double3 B = tri.b - origin;
    precise double3 C = tri.c - origin;
    uint x = axesAndCount.x, y = axesAndCount.y, z = axesAndCount.z;
    precise double Ax = Component(A, x) - shearAndMax.x * Component(A, z);
    precise double Ay = Component(A, y) - shearAndMax.y * Component(A, z);
    precise double Bx = Component(B, x) - shearAndMax.x * Component(B, z);
    precise double By = Component(B, y) - shearAndMax.y * Component(B, z);
    precise double Cx = Component(C, x) - shearAndMax.x * Component(C, z);
    precise double Cy = Component(C, y) - shearAndMax.y * Component(C, z);
    precise double U = Cx * By - Cy * Bx;
    precise double V = Ax * Cy - Ay * Cx;
    precise double W = Bx * Ay - By * Ax;
    double edgeError = guard.y * (Magnitude(Cx * By) + Magnitude(Cy * Bx) +
        Magnitude(Ax * Cy) + Magnitude(Ay * Cx) + Magnitude(Bx * Ay) + Magnitude(By * Ax) + 1.0);
    bool negative = U < -edgeError || V < -edgeError || W < -edgeError;
    bool positive = U > edgeError || V > edgeError || W > edgeError;
    if (negative && positive) return 0;
    precise double det = U + V + W;
    if (Magnitude(det) <= edgeError) return 2;
    precise double Az = shearAndMax.z * Component(A, z);
    precise double Bz = shearAndMax.z * Component(B, z);
    precise double Cz = shearAndMax.z * Component(C, z);
    precise double D = U * Az + V * Bz + W * Cz;
    if (det < 0.0) { det = -det; D = -D; }
    precise double lower = inverseAndMin.w * det;
    double distanceError = guard.y * (Magnitude(U * Az) + Magnitude(V * Bz) + Magnitude(W * Cz) +
        Magnitude(lower) + 1.0);
    if (D < lower - distanceError) return 0;
    if (sceneAndFlags.z != 0) {
        precise double upper = shearAndMax.w * det;
        if (D > upper + distanceError) return 0;
        if (Magnitude(D - upper) <= distanceError) return 2;
    }
    if (Magnitude(D - lower) <= distanceError || Magnitude(U) <= edgeError ||
        Magnitude(V) <= edgeError || Magnitude(W) <= edgeError) return 2;
    return 1;
}
[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= axesAndCount.w) return;
    double3 origin = origins[thread.x];
    uint index = 0, result = 0, work = 0;
    [loop] while (index < sceneAndFlags.x) {
        if (++work > sceneAndFlags.w) { answers[thread.x] = 2; return; }
        Node node = nodes[index];
        if (!BoxHit(node, origin)) { index = node.escape; continue; }
        [loop] for (uint i = 0; i < node.count; ++i) {
            if (++work > sceneAndFlags.w) { answers[thread.x] = 2; return; }
            uint hit = TriangleHit(triangles[node.first + i], origin);
            if (hit == 1) { answers[thread.x] = 1; return; }
            if (hit == 2) result = 2;
        }
        ++index;
    }
    answers[thread.x] = result;
}
)hlsl";

} // namespace geomsrv::archviz

#endif
