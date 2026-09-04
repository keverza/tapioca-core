#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/LibraryPreviewCommands.hpp"

// EVP_ACAPI_FAIL arrives with CommandBase.hpp, which is the ONE recorded
// boundary for Diagnostics/ApiError.hpp in this tier - every other command
// translation unit reaches the macro the same way. Naming it here directly
// would be a second sideways include for a dependency the tier already has.
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include <cstdint>
#include <string>
#include <vector>

// The two preview verbs, and the one parser they share.
//
// ⚠️ TWO CONSUMERS THAT DECODE DIFFERENT THINGS, WHICH IS WHY BOTH VERBS EXIST.
// The palette draws thumbnails by handing the bytes straight to
// NewDisplay::NativeImage, which reads JPEG and PNG only - so
// GetLibraryPartPreviewInfo answers "could the palette draw this". A browser
// reads GIF, WEBP and BMP as well, and has a bus in the way, so
// GetLibraryPartPreview answers "here is something an <img> can show". Folding
// them into one verb would mean one `decodable` flag answering for two different
// decoders, and it would be wrong for one of them.

namespace geomsrv {
namespace {

// What FORMAT a library part's preview picture is stored in, and how big it is.
// Reads the header only — the bytes are not returned, because the question this
// answers is whether a thumbnail is DECODABLE AT ALL, and that is settled by the
// mime string alone.
//
// ⚠️ THE OPEN QUESTION THIS EXISTS FOR. The preview lives in the section the
// DevKit calls API_SectInfoGIF, whose payload is a NUL-terminated MIME string
// followed by the raw image bytes (LibPart_Test's SetPreviewPictureToLibPart
// writes exactly that shape). But NewDisplay::NativeImage decodes JPEG and PNG
// ONLY — its Encoding enum has no GIF — so if the stock library really stores
// GIF, a thumbnail needs a decoder the DevKit does not ship, and that changes
// what the picker can offer. The section's NAME says GIF; the example's own
// comment says "prefer gif, but you can change gif to jpeg or png", so the name
// is legacy and the MIME STRING is the authority. Only a real library answers
// it, which is why this is reported rather than assumed.
// `bytes`, when given, receives the image payload itself. ONE parser for both
// verbs deliberately: the section's shape - a NUL-terminated MIME string then
// the image - is a contract, and a second copy of it in a preview-bytes command
// is the copy that keeps the off-by-one when this one is fixed.
void ReadPreviewFormat (Int32 libIndex, GS::UniString& mime, GS::Int32& byteCount,
                        std::vector<unsigned char>* bytes = nullptr)
{
    mime.Clear ();
    byteCount = 0;
    if (bytes != nullptr)
        bytes->clear ();

    API_LibPartSection section = {};
    section.sectType = API_SectInfoGIF;

    GSHandle sectionHdl = nullptr;
    if (ACAPI_LibraryPart_GetSection (libIndex, &section, &sectionHdl, nullptr) != NoError || sectionHdl == nullptr)
        return;

    const GSSize size = BMGetHandleSize (sectionHdl);
    if (size > 0) {
        // The MIME string comes first, NUL-terminated; the image follows it.
        // Bounded by the handle size rather than trusting the terminator — a
        // truncated section would otherwise walk off the end.
        const char* data = *sectionHdl;
        GSSize length = 0;
        while (length < size && data[length] != '\0')
            ++length;
        if (length < size) {
            // A LOCAL COPY, NUL-terminated by construction: the MIME text is a
            // slice of a larger handle, and GS::UniString has no "first N bytes
            // of this buffer" constructor — handing it the raw pointer would run
            // past the slice into the image bytes.
            GS::String text;
            for (GSSize i = 0; i < length; ++i)
                text += data[i];
            mime = GS::UniString (text);
            byteCount = (GS::Int32) (size - length - 1);
            if (bytes != nullptr && byteCount > 0) {
                const unsigned char* image = reinterpret_cast<const unsigned char*> (data + length + 1);
                bytes->assign (image, image + byteCount);
            }
        }
    }
    BMKillHandle (&sectionHdl);
}

// ---------------------------------------------------------------------------
// Tapioca.ListLibraryParts { subtype?, nameFilter?, limit? }
//
// What the loaded libraries actually contain THAT CAN BE PLACED. It exists
// because until now every command that wanted a library part had to HARD-CODE a
// candidate name list (PlaceLibraryObject's `libraryPartNames`), guessing at the
// spelling the installed library happens to use — "Slope Symbol" / "Slope Symbol
// 29" / "27". A command can now look instead of guess, and the palette's
// evp.LibraryPart picker is built on the same read.
//
// ⚠️ `subtype` OMITTED MEANS OBJECTS, NOT EVERYTHING. The first cut listed every
// registered library part, which put surfaces, images, lamps, section markers and
// templates in front of a user who had asked "which object do I place" — the
// report was *"data is all over the place"*. This now answers the question
// Archicad's own Object Settings browser answers, and each row carries the
// Library Manager `treePath` so a caller can show the folders the user knows.
//
// A pure READ: no undo step, MainThreadCommand.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Tapioca.GetLibraryPartPreviewInfo { name }
//
// ONE part's preview picture header — the format and the byte count, never the
// bytes. It exists to settle a single question before any thumbnail work is
// built on a guess: CAN a library part's preview be decoded with what the DevKit
// ships? See ReadPreviewFormat for why the answer is not knowable from the
// headers (the section is NAMED for GIF, its payload declares its own MIME, and
// NativeImage decodes only JPEG and PNG).
//
// Deliberately per-part rather than a column on ListLibraryParts: reading a
// section is a file touch, and doing it for every row of a multi-thousand-part
// catalogue would make the picker slow to answer a question that needs one part.
//
// A pure READ: no undo step, MainThreadCommand.
// ---------------------------------------------------------------------------
class GetLibraryPartPreviewInfoCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetLibraryPartPreviewInfo";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString name;
        if (!params.Get ("name", name) || name.IsEmpty ())
            return NativeCommandResult::Failure ("need name (the library part's name, as the Object tool shows it)");

        API_LibPart part = {};
        GS::ucscpy (part.docu_UName, name.ToUStr ());
        const GSErrCode searchErr = ACAPI_LibraryPart_Search (&part, false, true);
        delete part.location;
        if (searchErr != NoError) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_LibraryPart_Search", searchErr,
                                GS::UniString::Printf ("no placeable library part named \"%T\"", name.ToPrintf ())));
        }

        GS::UniString mime;
        GS::Int32 byteCount = 0;
        ReadPreviewFormat (part.index, mime, byteCount);

        GS::ObjectState os;
        os.Add ("name", name);
        // Empty mime with zero bytes means the part simply has no preview — a
        // normal state, not a failure, and one a thumbnail grid has to handle.
        os.Add ("previewMime", mime);
        os.Add ("previewBytes", byteCount);
        // ⚠️ THE ANSWER THE THUMBNAIL WORK IS WAITING ON. NewDisplay::NativeImage
        // decodes JPEG and PNG only, so this says whether the picker could render
        // this preview at all without a decoder the DevKit does not ship.
        const GS::UniString lowered = mime.ToLowerCase ();
        os.Add ("decodable", lowered == "image/png" || lowered == "image/jpeg" || lowered == "image/jpg");
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.GetLibraryPartPreview { name }
//
// ONE part's preview picture, as a data URI a web client can put in an <img>.
//
// ⚠️ THE PALETTE DOES NOT NEED THIS AND THE GRAPH EDITOR CANNOT WORK WITHOUT IT.
// Palette/CatalogBrowser draws thumbnails by calling LoadLibraryPartPreview
// straight into a NewDisplay::NativeImage on the main thread - no bytes ever
// cross anything. A browser-hosted picker has a bus between it and the library,
// so the bytes have to travel, and that is the whole reason this verb exists.
//
// ⚠️ AND THE BROWSER DECODES MORE THAN THE PALETTE CAN. NativeImage handles JPEG
// and PNG only, which is why GetLibraryPartPreviewInfo reports `decodable`
// against that pair; a WebView2 also renders GIF, WEBP and BMP, so this verb
// serves those too and the graph editor shows thumbnails the palette must leave
// blank. TIFF - which a real Archicad 29 library does ship - is refused by both,
// and is REPORTED as unrenderable rather than sent as bytes nothing can draw.
//
// ⚠️ ONE PART PER CALL, like its sibling. Reading a section touches the library
// file; a catalogue of four thousand parts must never be walked through this,
// and a client asks only for the cells actually on screen.
//
// A pure READ: no undo step, MainThreadCommand.
// ---------------------------------------------------------------------------

// Big enough for any thumbnail and small enough that one cannot become the
// response. A preview past this is reported as oversized rather than shipped:
// base64 inflates by a third, and a client asking for a grid of them would be
// asking for a multi-megabyte string it renders at 64 pixels square.
constexpr GS::Int32 kMaxPreviewBytes = 512 * 1024;

// Whether a browser can actually paint this. Not the same question as
// NativeImage's, and deliberately spelled separately from it.
bool BrowserRenderableMime (const GS::UniString& mime)
{
    const GS::UniString lowered = mime.ToLowerCase ();
    return lowered == "image/png" || lowered == "image/jpeg" || lowered == "image/jpg" || lowered == "image/gif" ||
           lowered == "image/webp" || lowered == "image/bmp";
}

class GetLibraryPartPreviewCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetLibraryPartPreview";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString name;
        if (!params.Get ("name", name) || name.IsEmpty ())
            return NativeCommandResult::Failure ("need name (the library part's name, as the Object tool shows it)");

        API_LibPart part = {};
        GS::ucscpy (part.docu_UName, name.ToUStr ());
        const GSErrCode searchErr = ACAPI_LibraryPart_Search (&part, false, true);
        delete part.location;
        if (searchErr != NoError) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_LibraryPart_Search", searchErr,
                                GS::UniString::Printf ("no placeable library part named \"%T\"", name.ToPrintf ())));
        }

        GS::UniString mime;
        GS::Int32 byteCount = 0;
        std::vector<unsigned char> bytes;
        ReadPreviewFormat (part.index, mime, byteCount, &bytes);

        // ⚠️ EVERY "NO PICTURE" OUTCOME IS A SUCCESS WITH A REASON, NOT A
        // FAILURE. A part with no preview section is the COMMON case, not an
        // edge; so is a TIFF one. A grid that got a command failure for either
        // would have to treat "this object has no thumbnail" as an error worth
        // showing, and a library where a third of the cells report errors is a
        // library that looks broken.
        GS::UniString reason, dataUri;
        if (byteCount <= 0 || bytes.empty ())
            reason = "this part has no preview picture";
        else if (!BrowserRenderableMime (mime))
            reason = GS::UniString::Printf ("its preview is %T, which a browser cannot draw", mime.ToPrintf ());
        else if (byteCount > kMaxPreviewBytes)
            reason = GS::UniString::Printf ("its preview is %d bytes, over the transfer cap", (int) byteCount);
        else
            dataUri = "data:" + mime + ";base64," + Base64Encode (bytes);

        GS::ObjectState os;
        os.Add ("name", name);
        os.Add ("previewMime", mime);
        os.Add ("previewBytes", byteCount);
        // Empty exactly when `reason` is not, so a client has one thing to test.
        os.Add ("dataUri", dataUri);
        os.Add ("reason", reason);
        return os;
    }
};

const NativeCommandRegistration LibraryPreviewCommandRegistrations[] = {
    { "GetLibraryPartPreviewInfo", &MakeRegisteredNativeCommand<GetLibraryPartPreviewInfoCommand>, false,
      R"json({"type":"object","properties":{"name":{"type":"string","minLength":1}},"additionalProperties":false,"required":["name"]})json",
      R"json({"type":"object","properties":{"name":{"type":"string"},"previewMime":{"type":"string"},"previewBytes":{"type":"integer"},"decodable":{"type":"boolean"}},"additionalProperties":false,"required":["name","previewMime","previewBytes","decodable"]})json" },

    { "GetLibraryPartPreview", &MakeRegisteredNativeCommand<GetLibraryPartPreviewCommand>, false,
      R"json({"type":"object","properties":{"name":{"type":"string","minLength":1}},"additionalProperties":false,"required":["name"]})json",
      R"json({"type":"object","properties":{"name":{"type":"string"},"previewMime":{"type":"string"},"previewBytes":{"type":"integer"},"dataUri":{"type":"string"},"reason":{"type":"string"}},"additionalProperties":false,"required":["name","previewMime","previewBytes","dataUri","reason"]})json" },
};

} // namespace

NativeCommandRegistrations GetLibraryPreviewCommandRegistrations ()
{
    return MakeRegistrationView (LibraryPreviewCommandRegistrations);
}

} // namespace geomsrv
