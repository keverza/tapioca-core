#include "ArchViz/MatrixMath.hpp"

#include <cmath>
#include <cstring>

namespace geomsrv {
namespace archviz {

void CameraBasis (const float eye[3], const float target[3],
                  float forward[3], float right[3], float up[3])
{
    for (int k = 0; k < 3; ++k)
        forward[k] = target[k] - eye[k];
    float length = std::sqrt (forward[0] * forward[0] + forward[1] * forward[1] +
                              forward[2] * forward[2]);
    if (length < 1e-6f) {
        forward[0] = 1.0f;
        forward[1] = 0.0f;
        forward[2] = 0.0f;
        length = 1.0f;
    }
    for (int k = 0; k < 3; ++k)
        forward[k] /= length;

    // Z-up world, like everything else in ArchViz.
    const float worldUp[3] = {0.0f, 0.0f, 1.0f};
    right[0] = worldUp[1] * forward[2] - worldUp[2] * forward[1];
    right[1] = worldUp[2] * forward[0] - worldUp[0] * forward[2];
    right[2] = worldUp[0] * forward[1] - worldUp[1] * forward[0];
    float rightLength = std::sqrt (right[0] * right[0] + right[1] * right[1] +
                                   right[2] * right[2]);
    if (rightLength < 1e-5f) {
        // Looking straight down: any basis perpendicular to the view will do.
        right[0] = 1.0f;
        right[1] = 0.0f;
        right[2] = 0.0f;
        rightLength = 1.0f;
    }
    for (int k = 0; k < 3; ++k)
        right[k] /= rightLength;

    up[0] = forward[1] * right[2] - forward[2] * right[1];
    up[1] = forward[2] * right[0] - forward[0] * right[2];
    up[2] = forward[0] * right[1] - forward[1] * right[0];
}

namespace {

void Cross (float out[3], const float a[3], const float b[3])
{
    const float x = a[1] * b[2] - a[2] * b[1];
    const float y = a[2] * b[0] - a[0] * b[2];
    const float z = a[0] * b[1] - a[1] * b[0];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

float Dot (const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Normalize (float v[3])
{
    const float lengthSq = Dot (v, v);
    if (lengthSq <= 0.0f)
        return;
    const float inv = 1.0f / std::sqrt (lengthSq);
    v[0] *= inv;
    v[1] *= inv;
    v[2] *= inv;
}

}   // namespace

void LookAtRH (float out[16], const float eye[3], const float at[3], const float up[3])
{
    // Right-handed: the camera's third axis points BACK along the line of
    // sight, so the scene lands at negative view-space z.
    float view[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
    Normalize (view);

    float right[3];
    Cross (right, up, view);
    if (Dot (right, right) == 0.0f) {
        // up is parallel to the line of sight -- looking straight down, which
        // an architectural viewer does constantly. bx picks +X here; without a
        // fallback the normalize below divides by zero and the whole matrix
        // becomes NaN, which renders as an empty viewport.
        right[0] = 1.0f;
        right[1] = 0.0f;
        right[2] = 0.0f;
    } else {
        Normalize (right);
    }

    float trueUp[3];
    Cross (trueUp, view, right);

    out[0]  = right[0];  out[1]  = trueUp[0];  out[2]  = view[0];  out[3]  = 0.0f;
    out[4]  = right[1];  out[5]  = trueUp[1];  out[6]  = view[1];  out[7]  = 0.0f;
    out[8]  = right[2];  out[9]  = trueUp[2];  out[10] = view[2];  out[11] = 0.0f;
    out[12] = -Dot (right, eye);
    out[13] = -Dot (trueUp, eye);
    out[14] = -Dot (view, eye);
    out[15] = 1.0f;
}

void PerspectiveRH (float out[16], float fovYDegrees, float aspect,
                    float nearZ, float farZ)
{
    constexpr float kPi = 3.14159265358979323846f;
    const float height = 1.0f / std::tan (fovYDegrees * (kPi / 180.0f) * 0.5f);
    const float width = height / aspect;
    // Depth 0..1. The homogeneous (-1..1) variant bx also offers is deliberately
    // absent: see the header.
    const float aa = farZ / (farZ - nearZ);
    const float bb = nearZ * aa;

    std::memset (out, 0, sizeof (float) * 16);
    out[0] = width;
    out[5] = height;
    out[10] = -aa;
    // ⚠️ w = -z_view. This is what makes it right-handed, and it is why out[15]
    // stays 0: the projection is not affine.
    out[11] = -1.0f;
    out[14] = -bb;
}

void OrthographicRH (float out[16], float left, float right, float bottom, float top,
                     float nearZ, float farZ)
{
    // bx::mtxOrtho with handedness Right and homogeneousNdc false. Worked out
    // the same way PerspectiveRH was, and checked in tests/cpp/test_matrixmath.
    const float width = right - left;
    const float height = top - bottom;
    const float depth = farZ - nearZ;

    std::memset (out, 0, sizeof (float) * 16);
    out[0] = width != 0.0f ? 2.0f / width : 0.0f;
    out[5] = height != 0.0f ? 2.0f / height : 0.0f;
    // Negative, for the same reason PerspectiveRH's out[10] is: the scene sits
    // at negative view-space z, and clip z has to come out increasing into the
    // screen.
    out[10] = depth != 0.0f ? -1.0f / depth : 0.0f;
    out[12] = width != 0.0f ? (left + right) / -width : 0.0f;
    out[13] = height != 0.0f ? (top + bottom) / -height : 0.0f;
    out[14] = depth != 0.0f ? -nearZ / depth : 0.0f;
    // ⚠️ 1, NOT 0. An orthographic projection IS affine -- that is the whole
    // difference from PerspectiveRH, whose out[15] is 0 and whose w is -z.
    out[15] = 1.0f;
}

void Multiply (float out[16], const float a[16], const float b[16])
{
    float result[16];
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[row * 4 + k] * b[k * 4 + col];
            result[row * 4 + col] = sum;
        }
    }
    std::memcpy (out, result, sizeof (result));
}

void TransformPoint (float out[4], const float v[4], const float m[16])
{
    float result[4];
    for (int col = 0; col < 4; ++col)
        result[col] = v[0] * m[col] + v[1] * m[4 + col] + v[2] * m[8 + col] + v[3] * m[12 + col];
    std::memcpy (out, result, sizeof (result));
}

}   // namespace archviz
}   // namespace geomsrv
