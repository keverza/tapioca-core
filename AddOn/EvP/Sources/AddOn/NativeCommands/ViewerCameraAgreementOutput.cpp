// NativeCommands/ViewerCameraAgreementOutput -- see the header.

#include "NativeCommands/ViewerCameraAgreementOutput.hpp"

#include "ArchViz/Dxgi/CameraAgreement.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace geomsrv {

GS::ObjectState BuildCameraAgreementOutput ()
{
    namespace cameraagreement = geomsrv::archviz::dxgi::cameraagreement;

    // CAMERA AGREEMENT -- per image, the camera POSE of every Archicad draw
    // against the verified draw's and the model family's, plus the raw
    // windows of a few consecutive images.
    const cameraagreement::Stats agreement = cameraagreement::GetStats ();
    const auto text = [] (uint64_t value) { return GS::UniString (std::to_string (value).c_str (), CC_UTF8); };
    // ⚠️ %.9g, NOT std::to_string: a float needs nine significant digits to
    // round-trip, and to_string's six fixed decimals print 3/2449 as 0.001225.
    const auto real = [] (double value) {
        char buffer[32];
        std::snprintf (buffer, sizeof (buffer), "%.9g", value);
        return GS::UniString (buffer, CC_UTF8);
    };
    GS::ObjectState agreementOut;
    agreementOut.Add ("enabled", agreement.enabled);
    agreementOut.Add ("imagesOpened", text (agreement.imagesOpened));
    agreementOut.Add ("imagesSkipped", text (agreement.imagesSkipped));
    agreementOut.Add ("imagesRead", text (agreement.imagesRead));
    agreementOut.Add ("imagesConsecutive", text (agreement.imagesConsecutive));
    agreementOut.Add ("drawsTruncated", text (agreement.drawsTruncated));
    agreementOut.Add ("keysTruncated", text (agreement.keysTruncated));
    agreementOut.Add ("readbacksPending", text (agreement.readbacksPending));
    agreementOut.Add ("readbackFailures", text (agreement.readbackFailures));
    agreementOut.Add ("createFailures", text (agreement.createFailures));
    agreementOut.Add ("rootMissing", text (agreement.rootMissing));
    agreementOut.Add ("rootPoseMissing", text (agreement.rootPoseMissing));
    agreementOut.Add ("referenceMissing", text (agreement.referenceMissing));
    agreementOut.Add ("imagesMoving", text (agreement.imagesMoving));
    agreementOut.Add ("rootSame", text (agreement.rootSame));
    agreementOut.Add ("rootPrevious", text (agreement.rootPrevious));
    agreementOut.Add ("rootAhead", text (agreement.rootAhead));
    agreementOut.Add ("rootNeither", text (agreement.rootNeither));
    agreementOut.Add ("rootAngleSumDegrees", real (agreement.rootAngleSumDegrees));
    agreementOut.Add ("referenceStepSumDegrees", real (agreement.referenceStepSumDegrees));
    agreementOut.Add ("keyCount", GS::Int32 (agreement.keyCount));
    GS::Array<GS::ObjectState> keysOut;
    for (uint32_t j = 0; j < agreement.keyCount && j < cameraagreement::kMaxKeys; ++j) {
        const cameraagreement::KeyStats& key = agreement.keys[j];
        GS::ObjectState entry;
        entry.Add ("count", text (key.count));
        entry.Add ("ordinal", GS::Int32 (key.ordinal));
        entry.Add ("kind", GS::Int32 (key.kind));
        entry.Add ("seen", text (key.seen));
        entry.Add ("posed", text (key.posed));
        entry.Add ("poseWindow", GS::Int32 (key.poseWindow));
        GS::Array<GS::UniString> formsB1Out;
        GS::Array<GS::UniString> formsB2Out;
        for (size_t f = 0; f < cameraagreement::kFormCount; ++f) {
            formsB1Out.Push (text (key.formsB1[f]));
            formsB2Out.Push (text (key.formsB2[f]));
        }
        entry.Add ("formsB1", formsB1Out);
        entry.Add ("formsB2", formsB2Out);
        entry.Add ("moving", text (key.moving));
        entry.Add ("rootSame", text (key.rootSame));
        entry.Add ("rootPrevious", text (key.rootPrevious));
        entry.Add ("rootAhead", text (key.rootAhead));
        entry.Add ("rootNeither", text (key.rootNeither));
        entry.Add ("referenceMoving", text (key.referenceMoving));
        entry.Add ("vsReferenceSame", text (key.vsReferenceSame));
        entry.Add ("vsReferencePrevious", text (key.vsReferencePrevious));
        entry.Add ("vsReferenceAhead", text (key.vsReferenceAhead));
        entry.Add ("vsReferenceNeither", text (key.vsReferenceNeither));
        entry.Add ("wasRoot", text (key.wasRoot));
        entry.Add ("wasReference", text (key.wasReference));
        entry.Add ("stepSumDegrees", real (key.stepSumDegrees));
        entry.Add ("rootAngleSumDegrees", real (key.rootAngleSumDegrees));
        entry.Add ("rootEyeSum", real (key.rootEyeSum));
        entry.Add ("rootEyeSamples", text (key.rootEyeSamples));
        keysOut.Push (entry);
    }
    agreementOut.Add ("keys", keysOut);

    // Heap, not stack: four images of thirty-two draws is ~45 KB.
    std::vector<cameraagreement::DumpImage> dump (cameraagreement::kDumpImages);
    dump.resize (cameraagreement::CopyDump (dump.data (), dump.size ()));
    GS::Array<GS::ObjectState> dumpOut;
    for (const cameraagreement::DumpImage& image : dump) {
        GS::ObjectState imageOut;
        imageOut.Add ("generation", text (image.generation));
        GS::Array<GS::ObjectState> drawsOut;
        for (uint32_t k = 0; k < image.draws && k < cameraagreement::kMaxDraws; ++k) {
            const cameraagreement::DumpDraw& draw = image.draw[k];
            GS::ObjectState drawOut;
            drawOut.Add ("count", text (draw.count));
            drawOut.Add ("ordinal", GS::Int32 (draw.ordinal));
            drawOut.Add ("kind", GS::Int32 (draw.kind));
            drawOut.Add ("root", draw.root);
            drawOut.Add ("reference", draw.reference);
            drawOut.Add ("vertexShader", text (draw.vertexShader));
            drawOut.Add ("renderTarget", text (draw.renderTarget));
            GS::Array<GS::ObjectState> windowsOut;
            for (size_t s = 0; s < cameraagreement::kWindows; ++s) {
                const cameraagreement::DumpWindow& window = draw.windows[s];
                GS::ObjectState windowOut;
                windowOut.Add ("slot", GS::Int32 (s));
                windowOut.Add ("bound", window.bound);
                windowOut.Add ("form", GS::Int32 (window.form));
                windowOut.Add ("buffer", text (window.buffer));
                windowOut.Add ("firstConstant", text (window.firstConstant));
                windowOut.Add ("numConstants", text (window.numConstants));
                windowOut.Add ("bytesCopied", GS::Int32 (window.bytesCopied));
                GS::Array<GS::UniString> valuesOut;
                for (float value : window.values)
                    valuesOut.Push (real (value));
                windowOut.Add ("values", valuesOut);
                windowsOut.Push (windowOut);
            }
            drawOut.Add ("windows", windowsOut);
            drawsOut.Push (drawOut);
        }
        imageOut.Add ("draws", drawsOut);
        dumpOut.Push (imageOut);
    }
    agreementOut.Add ("dump", dumpOut);
    return agreementOut;
}

} // namespace geomsrv
