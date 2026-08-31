#include "ApiPipeProtocol.hpp"

#include <array>

namespace evp::dynamo::protocol {
namespace {

void AppendUint32 (std::vector<uint8_t>& bytes, uint32_t value)
{
    bytes.push_back ((uint8_t) (value & 0xff));
    bytes.push_back ((uint8_t) ((value >> 8) & 0xff));
    bytes.push_back ((uint8_t) ((value >> 16) & 0xff));
    bytes.push_back ((uint8_t) ((value >> 24) & 0xff));
}

uint32_t ReadUint32 (const uint8_t* bytes)
{
    return (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) | ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

} // namespace

bool DecodeRequestHeader (const uint8_t* bytes, size_t size, RequestSizes& sizes, std::string& error)
{
    if (bytes == nullptr || size != HeaderSize) {
        error = "The request header must contain exactly 8 bytes.";
        return false;
    }

    sizes.commandBytes = ReadUint32 (bytes);
    sizes.paramsBytes = ReadUint32 (bytes + 4);
    if (sizes.commandBytes == 0 || sizes.commandBytes > MaxCommandBytes) {
        error = "The command length is outside the supported range.";
        return false;
    }
    if (sizes.paramsBytes > MaxParamsBytes) {
        error = "The params JSON exceeds the supported size.";
        return false;
    }
    return true;
}

bool IsAllowedCommand (std::string_view command)
{
    // Reads support preview and edit staging. The two model writes are explicit Apply
    // operations; arbitrary dispatcher access never crosses the pipe.
    //
    // Tapioca.SetSelection is a THIRD kind: it writes the user's selection, not the
    // model. It is admitted for one reason — the selection-set node's Reselect button
    // (TapiocaDynamoNodes/SelectionSetNode.cs), which is a deliberate press and not an
    // evaluation. NodeGraphSelectionCommands.cpp draws exactly that line for the
    // node-graph editor, and a graph that moved the selection just by evaluating would
    // be the defect both are written to prevent.
    constexpr std::array<std::string_view, 7> allowed { "Tapioca.GetSelection",      "Tapioca.GetModelElements",
                                                        "Tapioca.GetBodyGeometry",   "Tapioca.GetElementDetails",
                                                        "Tapioca.SetElementDetails", "Tapioca.SetSelection",
                                                        "Tapir.MoveElements" };
    for (std::string_view candidate : allowed) {
        if (candidate == command)
            return true;
    }
    return false;
}

std::vector<uint8_t> EncodeRequest (const std::string& command, const std::string& paramsJson)
{
    std::vector<uint8_t> bytes;
    bytes.reserve (HeaderSize + command.size () + paramsJson.size ());
    AppendUint32 (bytes, (uint32_t) command.size ());
    AppendUint32 (bytes, (uint32_t) paramsJson.size ());
    bytes.insert (bytes.end (), command.begin (), command.end ());
    bytes.insert (bytes.end (), paramsJson.begin (), paramsJson.end ());
    return bytes;
}

std::vector<uint8_t> EncodeResponse (const std::string& envelopeJson)
{
    std::vector<uint8_t> bytes;
    bytes.reserve (4 + envelopeJson.size ());
    AppendUint32 (bytes, (uint32_t) envelopeJson.size ());
    bytes.insert (bytes.end (), envelopeJson.begin (), envelopeJson.end ());
    return bytes;
}

bool DecodeResponseHeader (const uint8_t* bytes, size_t size, uint32_t& envelopeBytes, std::string& error)
{
    if (bytes == nullptr || size != 4) {
        error = "The response header must contain exactly 4 bytes.";
        return false;
    }
    envelopeBytes = ReadUint32 (bytes);
    if (envelopeBytes == 0 || envelopeBytes > MaxResponseBytes) {
        error = "The response length is outside the supported range.";
        return false;
    }
    return true;
}

} // namespace evp::dynamo::protocol
