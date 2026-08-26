#include <gtest/gtest.h>

#include "Dynamo/ApiPipeProtocol.hpp"

#include <array>

using namespace evp::dynamo;

TEST (ApiPipeProtocol, RequestUsesLittleEndianLengthsAndPreservesPayloads)
{
    const std::vector<uint8_t> bytes = protocol::EncodeRequest ("Tapioca.GetSelection", "{}");
    ASSERT_EQ (bytes.size (), protocol::HeaderSize + 20u + 2u);
    EXPECT_EQ (bytes[0], 20u);
    EXPECT_EQ (bytes[4], 2u);
    EXPECT_EQ (std::string (bytes.begin () + protocol::HeaderSize, bytes.end ()), "Tapioca.GetSelection{}");
}

TEST (ApiPipeProtocol, RequestHeaderRejectsEmptyAndOversizedCommands)
{
    std::array<uint8_t, protocol::HeaderSize> header {};
    protocol::RequestSizes sizes;
    std::string error;
    EXPECT_FALSE (protocol::DecodeRequestHeader (header.data (), header.size (), sizes, error));

    header[0] = 1;
    header[1] = 4;
    EXPECT_FALSE (protocol::DecodeRequestHeader (header.data (), header.size (), sizes, error));
}

TEST (ApiPipeProtocol, ValidRequestHeaderDecodesBothLengths)
{
    const std::vector<uint8_t> request = protocol::EncodeRequest ("EvP.ApiVersion", "{\"x\":1}");
    protocol::RequestSizes sizes;
    std::string error;
    ASSERT_TRUE (protocol::DecodeRequestHeader (request.data (), protocol::HeaderSize, sizes, error));
    EXPECT_EQ (sizes.commandBytes, 14u);
    EXPECT_EQ (sizes.paramsBytes, 7u);
}

TEST (ApiPipeProtocol, ResponsePreservesEnvelopeByteForByte)
{
    const std::string envelope = "{\"ok\":true,\"data\":{}}";
    const std::vector<uint8_t> response = protocol::EncodeResponse (envelope);
    uint32_t size = 0;
    std::string error;
    ASSERT_TRUE (protocol::DecodeResponseHeader (response.data (), 4, size, error));
    ASSERT_EQ (size, envelope.size ());
    EXPECT_EQ (std::string (response.begin () + 4, response.end ()), envelope);
    EXPECT_EQ (protocol::ResponseAck, 0x06u);
}

TEST (ApiPipeProtocol, ResponseHeaderRejectsTruncatedEmptyAndOversizedFrames)
{
    uint32_t size = 0;
    std::string error;
    const std::array<uint8_t, 4> empty {};
    EXPECT_FALSE (protocol::DecodeResponseHeader (empty.data (), 3, size, error));
    EXPECT_FALSE (protocol::DecodeResponseHeader (empty.data (), empty.size (), size, error));

    const std::array<uint8_t, 4> oversized { 1, 0, 0, 2 };
    EXPECT_FALSE (protocol::DecodeResponseHeader (oversized.data (), oversized.size (), size, error));
}
