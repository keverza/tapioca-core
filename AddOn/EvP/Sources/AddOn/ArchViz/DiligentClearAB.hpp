#ifndef EVP_ARCHVIZ_DILIGENTCLEARAB_HPP
#define EVP_ARCHVIZ_DILIGENTCLEARAB_HPP

// ArchViz/DiligentClearAB — the PLAT-RE22 diagnostic: does a clear reach the
// screen, and if not, whose fault is it?
//
// It clears the SAME back-buffer view twice in one frame -- once through
// Diligent, once through raw D3D11 on the borrowed native context -- and reads
// the centre pixel back after each. That separates in a single frame two causes
// of a black viewport that are otherwise indistinguishable from outside:
// Diligent's state and dispatch, or the D3D11 presentation path underneath it.
//
// ⚠️ IT ANSWERED ITS QUESTION AND IS KEPT ANYWAY. The black turned out to be
// VirtualBox and no Diligent code was at fault; on bare metal both arms pass.
// It stays because it rules out that entire class in one frame at the cost of
// one frame, on a machine nobody has tested yet -- which is every machine this
// add-on has not run on.
//
// This file is the RESOURCE half. The verdict logic -- decoding the DXGI format
// to a channel order and an sRGB transfer, and deciding whether the bytes read
// back are the colour asked for -- lives in ArchViz/DiligentClearCheck, is free
// of D3D11 and Diligent, and is covered by tests/cpp/test_diligentclear.cpp.
// Keeping them apart is what lets the sRGB reasoning be tested without a GPU.
//
// ⚠️ RENDER THREAD ONLY, like everything else in the Diligent viewport.

#include "ArchViz/DiligentClearCheck.hpp"

#include <cstdint>
#include <string>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
struct ISwapChain;
struct ITextureView;
}   // namespace Diligent

namespace geomsrv {
namespace archviz {

// The clear colours of the A/B. ⚠️ THEY MUST BE FAR APART IN EVERY CHANNEL, so a
// readback can never be ambiguous about WHICH clear it is looking at.
constexpr float kDiligentClearColor[4] = {0.02f, 0.72f, 1.0f, 1.0f};   // cyan
constexpr float kNativeClearColor[4]   = {1.0f, 0.05f, 0.60f, 1.0f};   // magenta
// sRGB encoding rounds; 2/255 is the same allowance Probe 1b used.
constexpr int kClearTolerance = 2;

// Who the device is, and whether it is still alive. Read on the way in and again
// on the way out: a device lost mid-run keeps the frame counter climbing, and
// only `deviceRemovedReason` says so.
struct DeviceDiagnostics {
    std::string adapter;
    uint32_t featureLevel = 0;
    uint64_t presentCount = 0;
    uint32_t deviceRemovedReason = 0;   // 0 == S_OK == alive
    std::string report;
};

DeviceDiagnostics DescribeDevice (Diligent::IRenderDevice* device,
                                  Diligent::ISwapChain* swapChain);

struct ClearAB {
    DeviceDiagnostics device;
    ClearVerdict diligentArm;
    ClearVerdict nativeArm;
    std::string verdict;
};

// ⚠️ THE CALLER MUST HAVE UNBOUND THE RENDER TARGET FIRST. D3D11 refuses to copy
// a resource still bound for output, and the first PLAT-RE22 diagnostic reported
// a zero pixel for exactly that reason rather than for a rendering fault.
//
// ⚠️ IT LEAVES THE MAGENTA ARM ON THE SURFACE. The caller re-clears to whatever
// the user is meant to be looking at.
ClearAB RunClearAB (Diligent::IDeviceContext* context, Diligent::ITextureView* rtv,
                    Diligent::ISwapChain* swapChain, Diligent::IRenderDevice* device);

}   // namespace archviz
}   // namespace geomsrv

#endif
