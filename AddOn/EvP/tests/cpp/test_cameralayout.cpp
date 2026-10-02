// ArchViz/Dxgi/CameraLayout -- where Archicad's camera is, and the pair every CPU
// scorer reads.
//
// WHAT THESE TESTS ARE FOR. The camera is `b1` (the view) and `b0` (its rotation
// times the projection); the image is `(p - eye) * b0`. The census read `b2` for two
// days: with the editing plane hidden and the camera orbiting, `b2` held a 2D screen
// map at every draw of every frame, and nothing locked. Every matrix below is REAL --
// the draw recorder's full-precision readbacks -- with Archicad's own ModelToScreen
// pixels for the model's corners at one still view. A test built from the same
// assumption as the code could not have caught this; these pin what Archicad wrote.

#include "ArchViz/Dxgi/CameraLayout.hpp"
#include "ArchViz/Dxgi/CameraShaderSource.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>

namespace cl = geomsrv::archviz::dxgi::cameralayout;
namespace cs = geomsrv::archviz::dxgi::camerashader;

namespace {

// 2026-09-27 12:05:59, plane hidden, still: the 1512-index model draw.
constexpr float kStillView[16] = { -0.348751128f, 0.132683977f,  -0.927775681f, 0.0f,
                                   -0.937215447f, -0.049373586f, 0.345238447f,  0.0f,
                                   0.0f,          0.989927948f,  0.14157255f,   0.0f,
                                   6.41576767f,   0.632916451f,  -66.4316025f,  1.0f };
constexpr float kStillB0[16] = { -0.454541087f, 0.329201519f,  0.965644062f,  0.927775681f,
                                 -1.22150981f,  -0.122500546f, -0.35932982f,  -0.345238447f,
                                 0.0f,          2.45610523f,   -0.147351012f, -0.14157255f,
                                 0.0f,          0.0f,          -0.204081625f, 0.0f };
constexpr float kStillB2[16] = { -0.454540998f, 0.32920149f,   0.930297494f, 0.927775621f, -1.22150958f,  -0.122500531f,
                                 -0.346176893f, -0.345238447f, 0.0f,         2.45610499f,  -0.141957358f, -0.141572535f,
                                 8.3619194f,    1.57032335f,   66.5119019f,  66.4316025f };

// The model's bounding-box corners and centre, and where Archicad's own
// ModelToScreen put them in that 2288x1202 window.
constexpr float kPoints[9][3] = {
    { -15.246232f, -15.965847f, 0.0f }, { -15.246232f, -15.965847f, 3.0f }, { -15.246232f, 41.089344f, 0.0f },
    { -15.246232f, 41.089344f, 3.0f },  { 47.940990f, -15.965847f, 0.0f },  { 47.940990f, -15.965847f, 3.0f },
    { 47.940990f, 41.089344f, 0.0f },   { 47.940990f, 41.089344f, 3.0f },   { 16.347379f, 12.561749f, 1.5f },
};
constexpr float kPixels[9][2] = { { 1833.0f, 617.0f }, { 1838.0f, 539.0f }, { 96.0f, 735.0f },
                                  { 84.0f, 619.0f },   { 1204.0f, 501.0f }, { 1204.0f, 463.0f },
                                  { 392.0f, 524.0f },  { 388.0f, 478.0f },  { 930.0f, 530.0f } };
constexpr float kWindowWidth = 2288.0f;
constexpr float kWindowHeight = 1202.0f;

// 16:16:05, plane hidden, still: the 1512-index draw. `b2` is view x projection.
constexpr float kHiddenB0[16] = { 0.462916404f, -1.4779706f,  -0.749820709f, -0.720416009f,
                                  1.21836042f,  0.561555386f, 0.284894615f,  0.273722291f,
                                  3.9e-08f,     1.91209066f,  -0.663251519f, -0.637241662f,
                                  0.0f,         0.0f,         -0.204081625f, 0.0f };
constexpr float kHiddenView[16] = { 0.355177194f, -0.595692933f, 0.720416009f,  0.0f,
                                    0.934799075f, 0.226333693f,  -0.273722291f, 0.0f,
                                    3.0e-08f,     0.770664096f,  0.637241662f,  0.0f,
                                    -29.3493671f, 4.13560104f,   -77.1953964f,  1.0f };
constexpr float kHiddenB2[16] = { 0.462916344f, -1.47797036f, -0.720591903f, -0.720416009f, 1.21836019f,  0.561555266f,
                                  0.273789108f, 0.273722291f, 1.5e-08f,      1.91209066f,   -0.63739717f, -0.637241602f,
                                  -38.252182f,  10.2608089f,  77.1806641f,   77.1953964f };

// 16:16:10, plane shown, still: the editing plane's 6-index quad. `b2` is the
// projection, and the camera the census locked on for two days was `b1 * b2`.
constexpr float kShownB0[16] = { 1.30227435f,    0.0422415435f, 0.0381560065f, 0.0366596915f,
                                 -0.0526812933f, 1.04420388f,   0.943211436f,  0.906222761f,
                                 -3.0e-08f,      2.25026369f,   -0.438400507f, -0.421208352f,
                                 0.0f,           0.0f,          -0.204081625f, 0.0f };
constexpr float kShownView[16] = { 0.99918282f,    0.0170253646f, -0.0366596915f, 0.0f,
                                   -0.0404202417f, 0.420864135f,  -0.906222761f,  0.0f,
                                   -2.3e-08f,      0.906964004f,  0.421208352f,   0.0f,
                                   -27.626709f,    -8.52883339f,  -56.5498314f,   1.0f };
constexpr float kShownB2[16] = { 1.30333924f, 0.0f, 0.0f,         0.0f,  0.0f, 2.48109484f, 0.0f,        0.0f,
                                 0.0f,        0.0f, -1.05842161f, -1.0f, 0.0f, 0.0f,        -7.5613327f, 0.0f };

// 21:00:33, plane hidden, ORBITING, frame 0 of the capture: the 1512-index draw,
// and the 24-index helper of the same frame, whose `b1` is the previous image's.
constexpr float kOrbitB0[16] = { -0.388509065f, 2.13124418f,   0.433252186f,  0.416261911f,
                                 -1.2440877f,   -0.665554166f, -0.135297894f, -0.129992098f,
                                 3.9e-08f,      1.08197343f,   -0.936635196f, -0.89990443f,
                                 0.0f,          0.0f,          -0.204081625f, 0.0f };
constexpr float kOrbitView[16] = { -0.298087418f, 0.858993471f,  -0.416261911f, 0.0f,
                                   -0.954538584f, -0.268250197f, 0.129992098f,  0.0f,
                                   3.0e-08f,      0.436087072f,  0.89990443f,   0.0f,
                                   2.97532129f,   6.94378281f,   -131.81601f,   1.0f };
constexpr float kOrbitB2[16] = { 0.000874125981f, 0.0f, 0.0f,  0.0f, 0.0f, -0.001663893f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 -1.0f,           0.0f, -1.0f, 1.0f, 0.0f, 1.0f };
constexpr float kOrbitHelperView[16] = { -0.292840332f, 0.860453904f,  -0.416969657f, 0.0f,
                                         -0.956161439f, -0.263528317f, 0.12770392f,   0.0f,
                                         3.0e-08f,      0.436087161f,  0.89990443f,   0.0f,
                                         3.07004213f,   6.92071152f,   -131.804825f,  1.0f };

constexpr float kParallel[16] = { 0.05f, 0.0f, 0.0f,   0.0f, 0.0f, 0.1f, 0.0f,  0.0f,
                                  0.0f,  0.0f, -0.01f, 0.0f, 0.0f, 0.0f, -0.5f, 1.0f };

void Multiply (const float a[16], const float b[16], float out[16])
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = sum;
        }
    }
}

void Clip (const float point[3], const float m[16], float clip[4])
{
    for (int c = 0; c < 4; ++c)
        clip[c] = point[0] * m[c] + point[1] * m[4 + c] + point[2] * m[8 + c] + m[12 + c];
}

// The worst distance from Archicad's own pixels, as a fraction of the window, for
// `p * view * projection`.
float WorstError (const float view[16], const float projection[16])
{
    float m[16];
    Multiply (view, projection, m);
    float worst = 0.0f;
    for (int i = 0; i < 9; ++i) {
        float clip[4];
        Clip (kPoints[i], m, clip);
        if (!(clip[3] > 1e-6f))
            return 1e9f;
        const float u = (clip[0] / clip[3] + 1.0f) * 0.5f;
        const float v = (1.0f - clip[1] / clip[3]) * 0.5f;
        worst = std::fmax (worst, std::fabs (u - kPixels[i][0] / kWindowWidth));
        worst = std::fmax (worst, std::fabs (v - kPixels[i][1] / kWindowHeight));
    }
    return worst;
}

// The largest difference in x, y and w -- the image -- between T(-eye) * b0 and a
// camera Archicad verified, relative to that camera's largest element.
float ImageDifference (const float b0[16], const float view[16], const float reference[16])
{
    double drawn[16];
    cl::ViewProjection (b0, view, drawn);
    float scale = 0.0f;
    for (int i = 0; i < 16; ++i)
        scale = std::fmax (scale, std::fabs (reference[i]));
    float worst = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int c : { 0, 1, 3 })
            worst = std::fmax (worst, std::fabs (float (drawn[r * 4 + c]) - reference[r * 4 + c]));
    }
    return worst / scale;
}

const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

// 2026-10-02, the draw recorder at one still camera: 16:14:47 in perspective, 16:14:56 the
// same camera in two-point perspective. The 12144-index model draw's `b1`, `b0` and `b2`,
// and where Archicad's own ModelToScreen put the model's corners and centre in the
// 2118x1206 window (16000 is ModelToScreen's off-screen answer).
constexpr float kPerspectiveView[16] = { 0.947874248f, -0.040562473f, 0.316052437f,  0.0f,
                                         0.318644732f, 0.120661736f,  -0.940162897f, 0.0f,
                                         -1.04e-07f,   0.991864622f,  0.127297163f,  0.0f,
                                         -685.595337f, 43.8004379f,   -590.43512f,   1.0f };
constexpr float kPerspectiveB0[16] = { 1.23541057f,  -0.092852734f, -0.328952521f, -0.316052437f,
                                       0.415305138f, 0.276210278f,  0.978536904f,  0.940162897f,
                                       -1.36e-07f,   2.27050614f,   -0.132492959f, -0.127297163f,
                                       0.0f,         0.0f,          -0.204081625f, 0.0f };
constexpr float kPerspectiveB2[16] = { 1.23541069f,  -0.092852719f, -0.31612885f,  -0.316051692f,
                                       0.415304095f, 0.276210219f,  0.940392673f,  0.940163136f,
                                       0.0f,         2.2705059f,    -0.127328232f, -0.127297148f,
                                       -893.570251f, 100.264824f,   590.437622f,   590.434631f };
constexpr float kTwoPointView[16] = { 0.947874248f,  0.0f,        0.318644732f, 0.0f, 0.318644732f, 0.0f,
                                      -0.947874248f, 0.0f,        0.0f,         1.0f, 0.0f,         0.0f,
                                      -685.595337f,  -31.716547f, -591.207397f, 1.0f };
constexpr float kTwoPointB0[16] = { 1.22536015f,  0.0f,         -0.331650645f, -0.318644732f, 0.411926538f, 0.0f,
                                    0.986562967f, 0.947874248f, 0.0f,          2.27050614f,   0.0f,         0.0f,
                                    0.0f,         0.0f,         -0.204081625f, 0.0f };
constexpr float kTwoPointB2[16] = { 1.22536027f,  -0.092852712f, -0.318721741f, -0.318643957f,
                                    0.411925465f, 0.276210219f,  0.948105872f,  0.947874486f,
                                    0.0f,         2.27050567f,   0.0f,          0.0f,
                                    -886.30072f,  100.264839f,   591.210693f,   591.206909f };
constexpr float kSitePoints[9][3] = {
    { 421.989746f, -388.515656f, -24.5f }, { 421.989746f, -388.515656f, 48.0f },  { 421.989746f, 118.421326f, -24.5f },
    { 421.989746f, 118.421326f, 48.0f },   { 952.254272f, -388.515656f, -24.5f }, { 952.254272f, -388.515656f, 48.0f },
    { 952.254272f, 118.421326f, -24.5f },  { 952.254272f, 118.421326f, 48.0f },   { 687.122009f, -135.047165f, 11.75f },
};
constexpr float kPerspectivePixels[9][2] = { { -4894.0f, 1250.0f }, { -5535.0f, 161.0f },   { 460.0f, 563.0f },
                                             { 451.0f, 386.0f },    { 16000.0f, 16000.0f }, { 16000.0f, -16000.0f },
                                             { 1929.0f, 620.0f },   { 1950.0f, 368.0f },    { 623.0f, 539.0f } };
constexpr float kTwoPointPixels[9][2] = { { -5275.0f, 1297.0f }, { -5275.0f, 175.0f },   { 463.0f, 563.0f },
                                          { 463.0f, 388.0f },    { 16000.0f, 16000.0f }, { 16000.0f, -16000.0f },
                                          { 1931.0f, 620.0f },   { 1931.0f, 372.0f },    { 626.0f, 539.0f } };
constexpr float kSiteWidth = 2118.0f;
constexpr float kSiteHeight = 1206.0f;

// The worst pixel distance, over the points Archicad put inside the window, between
// Archicad's ModelToScreen and the overlay's image: `(p - eye) * b0`, moved by `shift`.
double WorstPixelError (const float view[16], const float b0[16], const double shift[2], const float pixels[9][2])
{
    double eye[3];
    cl::Eye (view, eye);
    double worst = 0.0;
    for (int p = 0; p < 9; ++p) {
        if (!(pixels[p][0] >= 0.0f && pixels[p][0] <= kSiteWidth && pixels[p][1] >= 0.0f &&
              pixels[p][1] <= kSiteHeight))
            continue;
        double clip[4];
        for (int c = 0; c < 4; ++c)
            clip[c] = (kSitePoints[p][0] - eye[0]) * b0[c] + (kSitePoints[p][1] - eye[1]) * b0[4 + c] +
                      (kSitePoints[p][2] - eye[2]) * b0[8 + c] + b0[12 + c];
        clip[0] += shift[0] * clip[3];
        clip[1] += shift[1] * clip[3];
        const double x = (clip[0] / clip[3] + 1.0) * 0.5 * kSiteWidth;
        const double y = (1.0 - clip[1] / clip[3]) * 0.5 * kSiteHeight;
        worst = std::fmax (worst, std::fmax (std::fabs (x - pixels[p][0]), std::fabs (y - pixels[p][1])));
    }
    return worst;
}

} // namespace

// ⚠️ THE ACCEPTANCE TEST OF THE WHOLE DECODE: the pair the census scores must put
// the model where Archicad does -- and `b0` without the eye must not.
TEST (CameraLayout, TheRelativeCameraPutsTheModelWhereArchicadDoes)
{
    float decoded[16];
    ASSERT_EQ (cl::Decode (kStillView, kStillB0, decoded), cl::Layout::Relative);
    EXPECT_LT (WorstError (kStillView, decoded), 0.002f) << "within 0.2% of the window at nine model points";
    EXPECT_GT (WorstError (kIdentity, kStillB0), 0.5f) << "b0 is camera-relative: without the eye it is nowhere";
}

// The same image as the camera the census used to read, in every state measured.
TEST (CameraLayout, B0AndB1ReproduceTheVerifiedCameraInEveryState)
{
    EXPECT_LT (ImageDifference (kStillB0, kStillView, kStillB2), 1e-5f) << "plane hidden, 12:05:59";
    EXPECT_LT (ImageDifference (kHiddenB0, kHiddenView, kHiddenB2), 1e-5f) << "plane hidden, 16:16:05";
    float product[16];
    Multiply (kShownView, kShownB2, product);
    EXPECT_LT (ImageDifference (kShownB0, kShownView, product), 1e-5f) << "plane shown, the census's old b1 * b2";
}

// 21:00:33: the state that never locked. `b2` is a pixel-to-NDC map; the camera is
// still there, and the model's centre projects into the window.
TEST (CameraLayout, WhileOrbitingB2IsAScreenMapAndTheCameraStillDecodes)
{
    EXPECT_FALSE (cl::IsViewProjection (kOrbitB2)) << "b2 is not a camera";
    EXPECT_EQ (cl::RotationMismatch (kOrbitB0, kOrbitView), 0.0) << "one camera: the same floats";
    float decoded[16];
    ASSERT_EQ (cl::Decode (kOrbitView, kOrbitB0, decoded), cl::Layout::Relative);
    float m[16];
    Multiply (kOrbitView, decoded, m);
    float clip[4];
    Clip (kPoints[8], m, clip);
    ASSERT_GT (clip[3], 0.0f);
    EXPECT_LT (std::fabs (clip[0] / clip[3]), 1.0f);
    EXPECT_LT (std::fabs (clip[1] / clip[3]), 1.0f);
    EXPECT_GT (clip[2] / clip[3], 0.0f);
    EXPECT_LT (clip[2] / clip[3], 1.0f);
}

// The helper's `b1` is the previous image's camera; `b0` is this image's rotation.
TEST (CameraLayout, ThePreviousImagesHelperIsRefusedWhileTheCameraTurns)
{
    EXPECT_GT (cl::RotationMismatch (kOrbitB0, kOrbitHelperView), 1e-3);
    float decoded[16];
    EXPECT_EQ (cl::Decode (kOrbitHelperView, kOrbitB0, decoded), cl::Layout::Neither);
}

TEST (CameraLayout, OurDepthEnclosesTheModel)
{
    float decoded[16];
    ASSERT_EQ (cl::Decode (kStillView, kStillB0, decoded), cl::Layout::Relative);
    float m[16];
    Multiply (kStillView, decoded, m);
    for (int i = 0; i < 9; ++i) {
        float clip[4];
        Clip (kPoints[i], m, clip);
        EXPECT_GT (clip[2] / clip[3], 0.0f) << "point " << i;
        EXPECT_LT (clip[2] / clip[3], 1.0f) << "point " << i;
        EXPECT_NEAR (clip[2] / clip[3], float (cl::kDepthA + cl::kDepthB / clip[3]), 1e-5f) << "point " << i;
    }
}

// ⚠️ THE SHADER AND THE SCORER COMPUTE ONE THING. `camerashader::Compose` writes
// `(world - eye) * Projection` with `eye = -View[3].xyz * transpose (View3x3)`; the
// census scores `view * decoded`. Emulated here in the shader's own terms.
TEST (CameraLayout, TheShaderFormulaIsTheDecode)
{
    float decoded[16];
    ASSERT_EQ (cl::Decode (kOrbitView, kOrbitB0, decoded), cl::Layout::Relative);
    float m[16];
    Multiply (kOrbitView, decoded, m);
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = -(kOrbitView[12] * kOrbitView[i * 4 + 0] + kOrbitView[13] * kOrbitView[i * 4 + 1] +
                   kOrbitView[14] * kOrbitView[i * 4 + 2]);
    for (int p = 0; p < 9; ++p) {
        const float relative[3] = { kPoints[p][0] - eye[0], kPoints[p][1] - eye[1], kPoints[p][2] - eye[2] };
        float shader[4];
        Clip (relative, kOrbitB0, shader);
        shader[2] = float (cl::kDepthA) * shader[3] + float (cl::kDepthB);
        float scored[4];
        Clip (kPoints[p], m, scored);
        for (int c = 0; c < 4; ++c)
            EXPECT_NEAR (shader[c], scored[c], 1e-3f * (1.0f + std::fabs (scored[c]))) << "point " << p << " c " << c;
    }
}

TEST (CameraLayout, NothingThatIsNotOneCameraDecodes)
{
    float decoded[16];
    EXPECT_EQ (cl::Decode (kOrbitView, kOrbitB2, decoded), cl::Layout::Neither) << "a screen map in the window";
    EXPECT_EQ (std::memcmp (decoded, kOrbitB2, sizeof (decoded)), 0) << "passed through unchanged";
    EXPECT_EQ (cl::Decode (kStillView, kParallel, decoded), cl::Layout::Neither) << "parallel stays unsupported";
    EXPECT_EQ (cl::Decode (kStillView, kStillB2, decoded), cl::Layout::Neither) << "view x projection has an eye";
    EXPECT_EQ (cl::Decode (kStillB0, kStillB0, decoded), cl::Layout::Neither) << "b1 must be a rigid view";
    float scaled[16];
    std::memcpy (scaled, kStillView, sizeof (scaled));
    for (int i = 0; i < 11; ++i)
        scaled[i] *= 1.1f;
    EXPECT_EQ (cl::Decode (scaled, kStillB0, decoded), cl::Layout::Neither) << "a scaled view is not rigid";
    const float singular[16] = {};
    EXPECT_EQ (cl::Decode (singular, kStillB0, decoded), cl::Layout::Neither);
}

TEST (CameraLayout, TheGateWantsEverySampleDecoded)
{
    EXPECT_TRUE (cl::Decodes (32, 32));
    EXPECT_FALSE (cl::Decodes (32, 31)) << "one screen map among the samples";
    EXPECT_FALSE (cl::Decodes (0, 0));
}

TEST (CameraLayout, TheRelativeLayoutHasOneReading)
{
    EXPECT_EQ (cl::Interpretation (0, false), 0u);
    EXPECT_EQ (cl::Interpretation (2, false), 2u);
    EXPECT_EQ (cl::Interpretation (2, true), cl::kRelative) << "the eye is derived from the row-vector reading";
    EXPECT_TRUE (cl::IsRelative (cl::Interpretation (0, true)));
    EXPECT_FALSE (cl::IsRelative (0xffffffffu)) << "no interpretation is not a layout";
    EXPECT_EQ (cl::kProjectionWindow, 0u) << "the projection is copied from b0";
}

TEST (CameraShaderSource, EverySlotRoundTrips)
{
    for (uint32_t slot = 0; slot < cs::kShaderSlots; ++slot) {
        const uint32_t interpretation = cs::InterpretationOfSlot (slot);
        EXPECT_TRUE (cs::Declarable (interpretation));
        EXPECT_EQ (cs::SlotOf (interpretation), slot);
    }
    for (uint32_t reversed = 4; reversed < 8; ++reversed)
        EXPECT_FALSE (cs::Declarable (reversed)) << "a reversed multiplication order has no declaration";
    EXPECT_FALSE (cs::Declarable (0xffffffffu));
}

TEST (CameraShaderSource, AnUndeclarableOrMissingShaderFallsBackToVariantZero)
{
    int dummy = 0;
    int* shaders[cs::kShaderSlots] = {};
    shaders[0] = &dummy;
    shaders[cs::SlotOf (cl::kRelative)] = &dummy;
    EXPECT_EQ (cs::SlotToBind (cl::kRelative, shaders), cs::SlotOf (cl::kRelative));
    EXPECT_EQ (cs::SlotToBind (2u, shaders), 0u) << "missing shader";
    EXPECT_EQ (cs::SlotToBind (5u, shaders), 0u) << "reversed order";
    EXPECT_EQ (cs::SlotToBind (0xffffffffu, shaders), 0u) << "nothing selected";
}

// ⚠️ FINDING 1 AMENDED (2026-10-02): two-point perspective's image is `(p - eye) * b0`
// moved along w by a shift that only `b2` carries. Unshifted it lands 175.9 px below
// Archicad; shifted, where Archicad does.
TEST (CameraLayout, TwoPointPerspectivesShiftIsInB2AndPutsTheModelWhereArchicadDoes)
{
    double shift[2] = {};
    ASSERT_TRUE (cl::LensShift (kTwoPointB0, kTwoPointB2, shift));
    EXPECT_NEAR (shift[0], 0.0, 1e-5);
    EXPECT_NEAR (shift[1], 0.2914, 1e-4) << "the 3-point camera's y: -fy * tan (-7.31 deg)";
    EXPECT_LT (WorstPixelError (kTwoPointView, kTwoPointB0, shift, kTwoPointPixels), 1.0);
    const double none[2] = {};
    EXPECT_GT (WorstPixelError (kTwoPointView, kTwoPointB0, none, kTwoPointPixels), 170.0) << "what the overlay drew";
    float decoded[16];
    EXPECT_EQ (cl::Decode (kTwoPointView, kTwoPointB0, decoded), cl::Layout::Relative) << "the level camera decodes";
}

TEST (CameraLayout, AStill3PointViewHasNoShift)
{
    double shift[2] = { 1.0, 1.0 };
    ASSERT_TRUE (cl::LensShift (kPerspectiveB0, kPerspectiveB2, shift));
    EXPECT_NEAR (shift[0], 0.0, 1e-5);
    EXPECT_NEAR (shift[1], 0.0, 1e-5);
    EXPECT_LT (WorstPixelError (kPerspectiveView, kPerspectiveB0, shift, kPerspectivePixels), 1.0);
    ASSERT_TRUE (cl::LensShift (kStillB0, kStillB2, shift)) << "2026-09-27, plane hidden";
    EXPECT_NEAR (shift[1], 0.0, 1e-5);
}

// What `b2` holds while the camera moves -- a screen map, or another image's camera --
// is not `b0` moved along w, and gives no shift.
TEST (CameraLayout, AScreenMapOrAnotherCameraGivesNoShift)
{
    double shift[2] = { 1.0, 1.0 };
    EXPECT_FALSE (cl::LensShift (kOrbitB0, kOrbitB2, shift)) << "21:00:33, orbiting";
    EXPECT_EQ (shift[0], 0.0);
    EXPECT_EQ (shift[1], 0.0);
    EXPECT_FALSE (cl::LensShift (kPerspectiveB0, kTwoPointB2, shift)) << "the other mode's camera";
    EXPECT_FALSE (cl::LensShift (kTwoPointB0, kPerspectiveB2, shift));
    EXPECT_FALSE (cl::LensShift (kStillB0, kIdentity, shift));
    const float empty[16] = {};
    EXPECT_FALSE (cl::LensShift (kTwoPointB0, empty, shift)) << "a b2 copy never made";
}

TEST (CameraShaderSource, EachLayoutComposesItsOwnClip)
{
    char separate[cs::kMaxSource] = {};
    char relative[cs::kMaxSource] = {};
    ASSERT_TRUE (cs::Compose (0u, "BODY", separate, sizeof (separate)));
    ASSERT_TRUE (cs::Compose (cl::kRelative, "BODY", relative, sizeof (relative)));
    const std::string a (separate), b (relative);
    EXPECT_NE (a.find ("return mul (mul (world, View), Projection);"), std::string::npos);
    EXPECT_NE (b.find ("float3 eye = -mul (View[3].xyz, transpose ((float3x3) View));"), std::string::npos);
    EXPECT_NE (b.find ("mul (float4 (world.xyz - eye * world.w, world.w), Projection); c.xy += "), std::string::npos)
        << "the point moved to the eye, then the lens shift";
    EXPECT_EQ (b.find ("mul (mul (world, View), Projection)"), std::string::npos) << "b0 already holds the rotation";
    EXPECT_NE (b.find ("c.xy += ArchicadLensShift () * c.w; c.z = "), std::string::npos) << "the shift, then our depth";
    EXPECT_NE (b.find ("float4x4 Projection; row_major float4x4 Composite; };"), std::string::npos)
        << "b2 beside b0, in the same layout";
    EXPECT_EQ (a.find ("c.xy += ArchicadLensShift ()"), std::string::npos) << "only the relative layout shifts";
    EXPECT_EQ (a.substr (a.size () - 4), "BODY");
    EXPECT_FALSE (cs::Compose (4u, "BODY", separate, sizeof (separate))) << "reversed order refused";
}
